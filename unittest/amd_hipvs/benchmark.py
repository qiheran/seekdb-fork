#!/usr/bin/env python3
"""End-to-end seekdb SQL build benchmark for the AMD hipVS backend.

The harness only drives a real seekdb server: it creates a table, loads FP32
vectors, times CREATE VECTOR INDEX through the first usable ANN query, and
checks Recall@K against exact float64 ground truth. It contains no GPU
simulator, and an explicit lib=hipvs index never silently falls back to CPU.
Until the GPU rounds have run on an AMD host, this file is tooling, not a result.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import sys
import time
import traceback
from datetime import datetime, timezone
import uuid

import numpy as np
import pymysql

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}\Z")
OPTION = re.compile(r"([a-z_][a-z0-9_]*)\s*=\s*([a-z_][a-z0-9_]*|[0-9]+)\Z", re.IGNORECASE)
HARDWARE_FIELDS = ("cpu_model", "cpu_threads", "cpu_affinity", "gpu_model",
                   "gpu_architecture", "rocm_version", "hipvs_version", "seekdb_commit")
BUILD_LOG = re.compile(
    r'\[seekdb-hipvs\] backend=hipvs vendor=AMD device="([^"\r\n]+)" '
    r'arch=(\S+) rows=([0-9]+) dim=([0-9]+) m=([0-9]+) '
    r'ef_construction=([0-9]+) gpu_compute=true cpu_fallback=false')
LOG_MAX_INTERVAL_BYTES = 512 * 1024 * 1024
LOG_MAX_LINE_BYTES = 1024 * 1024
LOG_READ_CHUNK_BYTES = 64 * 1024

CPU_WITH = "distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200"
GPU_WITH = "distance=l2,type=hnsw,lib=hipvs,m=16,ef_construction=200,ef_search=200"


def identifier(value: str) -> str:
    if not IDENTIFIER.fullmatch(value):
        raise ValueError("unsafe SQL identifier")
    return "`" + value + "`"


def parse_options(value: str) -> dict[str, str]:
    options = {}
    for item in value.split(","):
        match = OPTION.fullmatch(item.strip())
        if not match or match[1].lower() in options:
            raise ValueError("WITH options must be unique name=value pairs without SQL fragments")
        options[match[1].lower()] = match[2].lower()
    return options


def options_sql(options: dict[str, str]) -> str:
    value = ",".join(f"{key}={item}" for key, item in sorted(options.items()))
    parse_options(value)
    return value


def validate_options(options: dict[str, str], lib: str, k: int) -> None:
    if options.get("distance") != "l2" or options.get("type") != "hnsw":
        raise ValueError("this harness compares FP32 HNSW using L2 distance only")
    if options.get("lib") != lib:
        raise ValueError(f"expected lib={lib} in the WITH clause")
    for key in ("m", "ef_construction", "ef_search"):
        if not re.fullmatch(r"[0-9]+", options.get(key, "")):
            raise ValueError(f"explicit integer {key} is required")
    if not 5 <= int(options["m"]) <= 128:
        raise ValueError("m must be in [5,128]")
    if not int(options["m"]) < int(options["ef_construction"]) <= 10000:
        raise ValueError("ef_construction must be greater than m and at most 10000")
    if not k <= int(options["ef_search"]) <= 160000:
        raise ValueError("ef_search must be between K and 160000")


def validate_comparable(cpu: dict[str, str], gpu: dict[str, str], k: int) -> None:
    validate_options(cpu, "vsag", k)
    validate_options(gpu, "hipvs", k)
    for key in ("distance", "type", "m", "ef_construction", "ef_search"):
        if cpu[key] != gpu[key]:
            raise ValueError(f"CPU/GPU {key} parameters must match")


def generate_data(rows: int, dim: int, queries: int, seed: int, m: int,
                  dataset: str = "gaussian"):
    if rows <= 2 * m or not 1 <= dim <= 16384 or queries < 1:
        raise ValueError("hipVS requires rows > 2*m, 1<=dim<=16384 and queries>=1")
    rng = np.random.default_rng(seed)
    if dataset == "clustered":
        # Embedding-like structure: points concentrate around cluster centers.
        # i.i.d. Gaussian coordinates concentrate every pairwise distance, which
        # collapses HNSW recall with row count no matter which builder runs.
        clusters = max(16, rows // 1024)
        centers = (rng.standard_normal((clusters, dim)) * 2.0).astype(np.float32)
        vectors = centers[rng.integers(0, clusters, rows)]
        vectors += (rng.standard_normal((rows, dim)) * 0.4).astype(np.float32)
        query = centers[rng.integers(0, clusters, queries)]
        query += (rng.standard_normal((queries, dim)) * 0.4).astype(np.float32)
    elif dataset == "gaussian":
        vectors = rng.standard_normal((rows, dim)).astype(np.float32)
        query = rng.standard_normal((queries, dim)).astype(np.float32)
    else:
        raise ValueError(f"unsupported dataset: {dataset}")
    # Above 2**53, so an accidental float ID conversion changes the value.
    ids = (np.arange(rows, dtype=np.int64) * 104729) + (2**53 + 101)
    if ids[-1] > np.iinfo(np.int64).max:
        raise ValueError("document IDs exceed signed BIGINT")
    rng.shuffle(ids)
    return vectors, query, ids


def fingerprint(vectors, queries, ids) -> str:
    digest = hashlib.sha256()
    for array, dtype in ((vectors, "<f4"), (queries, "<f4"), (ids, "<i8")):
        array = np.ascontiguousarray(array, dtype=dtype)
        digest.update(np.asarray(array.shape, dtype="<i8").tobytes())
        digest.update(memoryview(array).cast("B"))
    return digest.hexdigest()


def exact_neighbors(vectors, queries, ids, k: int, chunk: int = 65536) -> list[list[int]]:
    if not 1 <= k <= len(vectors):
        raise ValueError("K must be between 1 and the row count")
    if not np.isfinite(vectors).all() or not np.isfinite(queries).all():
        raise ValueError("all coordinates must be finite")
    if len(set(map(int, ids))) != len(vectors):
        raise ValueError("document IDs must be unique")
    result = []
    for query in queries:
        distances = np.empty(len(vectors), dtype=np.float64)
        query64 = np.asarray(query, dtype=np.float64)
        for begin in range(0, len(vectors), chunk):
            block = np.asarray(vectors[begin:begin + chunk], dtype=np.float64)
            delta = block - query64
            distances[begin:begin + chunk] = np.einsum("ij,ij->i", delta, delta)
        order = np.lexsort((ids, distances))[:k]
        result.append([int(value) for value in ids[order]])
    return result


def assess_results(results, truth, allowed_ids, k: int) -> dict:
    if not truth or len(results) != len(truth):
        raise ValueError("result/query count mismatch")
    allowed = set(map(int, allowed_ids))
    recalls = []
    for rows, expected in zip(results, truth):
        if len(rows) != k:
            raise ValueError("ANN returned an incomplete result set")
        actual = []
        previous = -math.inf
        for doc_id, distance in rows:
            if not isinstance(doc_id, (int, np.integer)) or isinstance(doc_id, bool):
                raise ValueError("ANN document ID is not an exact integer")
            if int(doc_id) not in allowed:
                raise ValueError("ANN returned a document ID absent from the input")
            distance = float(distance)
            if not math.isfinite(distance) or distance < 0:
                raise ValueError("ANN returned an invalid L2 distance")
            if distance + 1e-6 < previous:
                raise ValueError("ANN returned unsorted distances")
            previous = distance
            actual.append(int(doc_id))
        if len(set(actual)) != k:
            raise ValueError("ANN returned duplicate IDs")
        recalls.append(len(set(actual).intersection(expected)) / k)
    return {"recall_at_k": statistics.mean(recalls), "min_query_recall": min(recalls),
            "queries": len(truth), "valid_ids": True}


def log_position(path: Path) -> tuple[int, int, int]:
    info = path.stat()
    return info.st_dev, info.st_ino, info.st_size


def scan_log_interval(source, length: int) -> tuple[str, str]:
    if not 0 <= length <= LOG_MAX_INTERVAL_BYTES:
        raise RuntimeError("unexpectedly large stderr interval; inspect the server log")
    digest = hashlib.sha256()
    remaining = length
    pending = b""
    candidate = None

    def inspect_line(line):
        nonlocal candidate
        if len(line) > LOG_MAX_LINE_BYTES:
            raise RuntimeError("oversized stderr line; inspect the server log")
        if b"[seekdb-hipvs]" in line:
            if candidate is not None:
                raise RuntimeError("ambiguous GPU execution records during one SQL CREATE")
            candidate = line.removesuffix(b"\r").decode("utf-8", errors="replace")

    while remaining:
        block = source.read(min(LOG_READ_CHUNK_BYTES, remaining))
        if not block:
            raise RuntimeError("unexpected end of stderr interval; the log was truncated")
        remaining -= len(block)
        digest.update(block)
        lines = (pending + block).split(b"\n")
        pending = lines.pop()
        for line in lines:
            inspect_line(line)
        if len(pending) > LOG_MAX_LINE_BYTES:
            raise RuntimeError("oversized stderr line; inspect the server log")
    if pending:
        inspect_line(pending)
    if candidate is None:
        raise RuntimeError("missing GPU execution record during SQL CREATE")
    return candidate, digest.hexdigest()


def evidence_from_log(path: Path, start: tuple[int, int, int], table: str, index: str,
                      rows: int, dim: int, options: dict[str, str]) -> dict:
    """Correlate one isolated SQL CREATE with the bridge record in new stderr bytes."""
    with path.open("rb") as source:
        info = os.fstat(source.fileno())
        if (info.st_dev, info.st_ino) != start[:2] or info.st_size < start[2]:
            raise RuntimeError("seekdb stderr rotated during timed index construction")
        length = info.st_size - start[2]
        source.seek(start[2])
        candidate, digest = scan_log_interval(source, length)
        after = os.fstat(source.fileno())
        current = path.stat()
        if ((after.st_dev, after.st_ino) != start[:2] or after.st_size < info.st_size
                or (current.st_dev, current.st_ino) != start[:2] or current.st_size < info.st_size):
            raise RuntimeError("seekdb stderr rotated or shrank while reading build evidence")
    match = BUILD_LOG.search(candidate)
    if (not match or (int(match[3]), int(match[4])) != (rows, dim)
            or (int(match[5]), int(match[6])) != (int(options["m"]), int(options["ef_construction"]))):
        raise RuntimeError("GPU execution record does not match the SQL input")
    return {"producer": "seekdb-hipvs-build", "backend": "hipvs", "device_vendor": "AMD",
            "gpu_compute": True, "cpu_fallback": False, "completed": True,
            "rows": rows, "dim": dim, "m": int(options["m"]),
            "ef_construction": int(options["ef_construction"]),
            "gpu_name": match[1], "gpu_architecture": match[2],
            "table_name": table, "index_name": index,
            "correlation": "single index build in dedicated server stderr during SQL CREATE",
            "stderr_start": start[2], "stderr_end": start[2] + length,
            "stderr_interval_bytes": length,
            "stderr_interval_sha256": digest, "raw_record": candidate}


def query_sql(table: str, index: str, k: int) -> str:
    identifier(table)
    identifier(index)
    return (f"SELECT /*+ INDEX(t {index}) */ id, l2_distance(v, %s) AS distance "
            f"FROM {identifier(table)} t ORDER BY l2_distance(v, %s) APPROXIMATE LIMIT {k}")


def vector_json(value) -> str:
    return json.dumps(np.asarray(value, dtype=np.float32).tolist(), separators=(",", ":"),
                      allow_nan=False)


def first_query_when_ready(connection, cursor, sql, values, max_retries=10):
    """Keep the baseline's post-DDL snapshot-visibility retries inside the timer."""
    failures = []
    while True:
        try:
            cursor.execute(sql, values)
            return cursor.fetchall(), failures
        except pymysql.err.MySQLError as error:
            code = error.args[0] if error.args else None
            if code not in (4016, 4138) or len(failures) >= max_retries:
                raise
            failures.append(code)
            connection.rollback()
            time.sleep(0.1)


def run_round(connection, report, mode, number, vectors, queries, ids, truth, options):
    cfg = report["config"]
    tag = report["run_id"]
    table = f"ahb_{tag}_{mode}_{number}"
    index = f"ahb_idx_{tag}_{mode}_{number}"
    created = False
    cursor = connection.cursor()
    try:
        cursor.execute("SET SESSION ob_hnsw_ef_search=%s", (int(options["ef_search"]),))
        cursor.execute("SELECT @@session.ob_hnsw_ef_search")
        if int(cursor.fetchone()[0]) != int(options["ef_search"]):
            raise RuntimeError("effective HNSW search breadth differs from the request")
        report["active_operation"] = f"{mode}/{number}: create raw table"
        cursor.execute(f"CREATE TABLE {identifier(table)} "
                       f"(id BIGINT PRIMARY KEY, v VECTOR({cfg['dim']}) NOT NULL)")
        created = True
        started = time.perf_counter()
        report["active_operation"] = f"{mode}/{number}: load rows"
        for begin in range(0, len(vectors), cfg["batch"]):
            stop = min(begin + cfg["batch"], len(vectors))
            rows = [(int(ids[i]), vector_json(vectors[i])) for i in range(begin, stop)]
            cursor.executemany(f"INSERT INTO {identifier(table)} (id,v) VALUES (%s,%s)", rows)
            connection.commit()
        load_sec = time.perf_counter() - started
        report["active_operation"] = f"{mode}/{number}: verify raw input"
        cursor.execute(f"SELECT COUNT(*), COUNT(DISTINCT id) FROM {identifier(table)}")
        counts = cursor.fetchone()
        if (int(counts[0]), int(counts[1])) != (len(ids), len(ids)):
            raise RuntimeError("raw row/ID count verification failed")
        cursor.execute(f"SHOW INDEX FROM {identifier(table)}")
        if any(row[2] != "PRIMARY" for row in cursor.fetchall()):
            raise RuntimeError("unexpected index before the timed build")
        sql = query_sql(table, index, cfg["k"])
        first = vector_json(queries[0])
        log_path = Path(cfg["server_stderr"]).resolve() if cfg.get("server_stderr") else None
        log_start = log_position(log_path) if mode == "gpu" and log_path else None
        # Start before DDL: scans, GPU transfer/sync, graph conversion and the
        # first usable ANN query are all inside this interval.
        started = time.perf_counter()
        report["active_operation"] = f"{mode}/{number}: create vector index"
        cursor.execute(f"CREATE VECTOR INDEX {identifier(index)} ON {identifier(table)}(v) "
                       f"WITH ({options_sql(options)})")
        ddl_sec = time.perf_counter() - started
        report["active_operation"] = f"{mode}/{number}: first ANN query"
        first_rows, readiness_retries = first_query_when_ready(connection, cursor, sql,
                                                               (first, first))
        assess_results([first_rows], truth[:1], ids, cfg["k"])
        build_sec = time.perf_counter() - started
        evidence = None
        evidence_error = None
        if log_start is not None:
            try:
                evidence = evidence_from_log(log_path, log_start, table, index,
                                             cfg["rows"], cfg["dim"], options)
            except Exception as error:
                evidence_error = str(error)
        report["active_operation"] = f"{mode}/{number}: explain vector index"
        cursor.execute("EXPLAIN " + sql, (first, first))
        plan = "\n".join(str(row[0]) for row in cursor.fetchall())
        vector_plan = "VECTOR INDEX" in plan.upper() and index.lower() in plan.lower()
        if not vector_plan:
            raise RuntimeError("EXPLAIN does not prove the requested vector index is used")
        report["active_operation"] = f"{mode}/{number}: recall queries"
        query_started = time.perf_counter()
        results = []
        for query in queries:
            parameter = vector_json(query)
            cursor.execute(sql, (parameter, parameter))
            results.append(cursor.fetchall())
        query_sec = time.perf_counter() - query_started
        quality = assess_results(results, truth, ids, cfg["k"])
        coverage = None
        samples = min(cfg["coverage_samples"], len(ids))
        if samples:
            report["active_operation"] = f"{mode}/{number}: self ANN diagnostic"
            self_sql = query_sql(table, index, 1)
            positions = np.linspace(0, len(ids) - 1, samples).astype(np.int64)
            found = 0
            examples = []
            for position in positions:
                parameter = vector_json(vectors[position])
                cursor.execute(self_sql, (parameter, parameter))
                own = cursor.fetchall()
                actual_id = int(own[0][0]) if own else None
                matched = own and actual_id == int(ids[position]) and abs(float(own[0][1])) < 1e-6
                found += bool(matched)
                if not matched and len(examples) < 20:
                    examples.append({"expected_id": int(ids[position]), "returned_id": actual_id})
            coverage = {"sampled": len(positions), "found": found, "missed_examples": examples}
        result = {"mode": mode, "round": number, "table_name": table, "index_name": index,
                  "raw_load_sec": load_sec, "ddl_sec": ddl_sec, "build_sec": build_sec,
                  "readiness_retry_codes": readiness_retries,
                  "query_ef_search": int(options["ef_search"]),
                  "query_sec": query_sec, "quality": quality,
                  "vector_plan_verified": vector_plan, "query_plan": plan,
                  "self_ann_coverage": coverage, "with": options}
        if evidence:
            result["gpu_evidence"] = evidence
        if evidence_error:
            result["gpu_evidence_error"] = evidence_error
        report["rounds"].append(result)
        print(json.dumps({"mode": mode, "round": number, "build_sec": build_sec,
                          "recall_at_k": quality["recall_at_k"],
                          "self_ann": None if coverage is None
                          else f"{coverage['found']}/{coverage['sampled']}"}), flush=True)
    finally:
        try:
            if created:
                cursor.execute(f"DROP TABLE {identifier(table)}")
        finally:
            cursor.close()


def summarize(report: dict) -> dict:
    rounds = report["rounds"]
    cfg = report["config"]
    cpu = [item for item in rounds if item["mode"] == "cpu"]
    gpu = [item for item in rounds if item["mode"] == "gpu"]
    summary = {"status": "INCOMPLETE", "speedup": None, "meets_target": False}
    if (len(cpu) != cfg["repeats"]
            or (not cfg["cpu_only"] and len(gpu) != cfg["repeats"])):
        if report.get("failure"):
            summary["reasons"] = [report["failure"]]
        return summary
    if any(not math.isfinite(item["build_sec"]) or item["build_sec"] <= 0 for item in rounds):
        raise ValueError("build durations must be finite and positive")
    failures = []
    for item in rounds:
        recall = item["quality"]["recall_at_k"]
        if not math.isfinite(recall) or not cfg["min_recall"] <= recall <= 1:
            failures.append(f"{item['index_name']}: recall below the configured threshold")
        if item.get("vector_plan_verified") is not True:
            failures.append(f"{item['index_name']}: requested vector index plan not verified")
    if failures:
        summary["status"] = "QUALITY_OR_FUNCTION_FAILED"
        summary["reasons"] = failures
        return summary
    summary["cpu_median_build_sec"] = statistics.median(item["build_sec"] for item in cpu)
    if cfg["cpu_only"]:
        summary["status"] = "CPU_ONLY"
        return summary
    summary["gpu_median_build_sec"] = statistics.median(item["build_sec"] for item in gpu)
    reasons = []
    if not cfg["skip_gpu_evidence"]:
        missing = [field for field in HARDWARE_FIELDS if not (report.get("server_environment") or {}).get(field)]
        if missing:
            reasons.append("missing server environment fields: " + ", ".join(missing))
        for item in gpu:
            if "gpu_evidence" not in item:
                reasons.append(f"{item['index_name']}: missing raw GPU build record"
                               + (f" ({item['gpu_evidence_error']})"
                                  if item.get("gpu_evidence_error") else ""))
    if reasons:
        summary.update(status="UNVERIFIED", reasons=reasons)
        return summary
    summary["speedup"] = summary["cpu_median_build_sec"] / summary["gpu_median_build_sec"]
    target = cfg["target_speedup"]
    if target is None:
        summary["status"] = "MEASURED_TARGET_UNSPECIFIED"
    else:
        summary["meets_target"] = summary["speedup"] >= target
        summary["status"] = "PASS" if summary["meets_target"] else "PERFORMANCE_TARGET_NOT_MET"
    return summary


def save_report(report: dict, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
                           encoding="utf-8")
    summary = report["summary"]
    lines = ["# AMD hipVS end-to-end build benchmark", "",
             f"Status: **{summary['status']}**", "",
             "Timed interval: CREATE VECTOR INDEX start through the first ANN query that uses "
             "the requested index. Data generation, raw load and quality checks are excluded.", "",
             "|path|round|load s|DDL s|build s|Recall@K|self ANN (diagnostic)|",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for row in report["rounds"]:
        coverage = row.get("self_ann_coverage")
        coverage_text = "-" if coverage is None else f"{coverage['found']}/{coverage['sampled']}"
        lines.append(f"|{row['mode']}|{row['round']}|{row['raw_load_sec']:.6f}|"
                     f"{row['ddl_sec']:.6f}|{row['build_sec']:.6f}|"
                     f"{row['quality']['recall_at_k']:.6f}|{coverage_text}|")
    lines += ["", f"CPU median: {summary.get('cpu_median_build_sec')} s.",
              f"GPU median: {summary.get('gpu_median_build_sec')} s.",
              f"Effective build speedup: "
              f"{summary.get('speedup') if summary.get('speedup') is not None else 'not verified'}.",
              f"Configured recall threshold: {report['config']['min_recall']}; "
              f"configured performance target: {report['config']['target_speedup']}.",
              "The target is supplied by the operator; this tool does not claim it is the "
              "official competition threshold.",
              "", "GPU claims require the raw bridge record from the timed index build "
              "(see gpu_evidence). CPU fallback is never counted as a GPU build.",
              "Full parameters, per-round records, data fingerprint and the server environment "
              "are in the JSON document next to this file."]
    if report.get("failure"):
        lines += ["", "The run failed; only completed rounds are preserved.", report["failure"]]
    destination.with_suffix(".md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def load_environment(path: Path | None) -> dict:
    if path is None:
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("environment JSON must be an object")
    return {key: data.get(key) for key in HARDWARE_FIELDS}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("AMD_BENCH_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("AMD_BENCH_PORT", "2881")))
    parser.add_argument("--user", default=os.environ.get("AMD_BENCH_USER", "root"))
    parser.add_argument("--password", default=os.environ.get("AMD_BENCH_PASSWORD", ""))
    parser.add_argument("--database", default=os.environ.get("AMD_BENCH_DATABASE", "test"))
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--dataset", choices=("gaussian", "clustered"), default="gaussian",
                        help="gaussian is i.i.d. noise (worst case); clustered mimics embeddings")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--batch", type=int, default=512)
    parser.add_argument("--cpu-with", default=CPU_WITH)
    parser.add_argument("--gpu-with", default=GPU_WITH)
    parser.add_argument("--cpu-only", action="store_true")
    parser.add_argument("--min-recall", type=float, default=0.90)
    parser.add_argument("--target-speedup", type=float)
    parser.add_argument("--coverage-samples", type=int, default=0)
    parser.add_argument("--server-stderr", type=Path)
    parser.add_argument("--environment-json", type=Path)
    parser.add_argument("--skip-gpu-evidence", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("local-results/benchmark.json"))
    args = parser.parse_args(argv)
    if args.repeats < 1 or args.queries < 1 or args.k < 1 or args.batch < 1:
        parser.error("repeats, queries, k and batch must be positive")
    if args.coverage_samples < 0:
        parser.error("--coverage-samples must be non-negative")
    if not 0 < args.min_recall <= 1:
        parser.error("--min-recall must be in (0,1]")
    if args.target_speedup is not None and args.target_speedup <= 0:
        parser.error("--target-speedup must be positive")
    if args.cpu_only and args.server_stderr:
        parser.error("--server-stderr is only used by GPU rounds")
    if args.server_stderr and args.skip_gpu_evidence:
        parser.error("--server-stderr and --skip-gpu-evidence are mutually exclusive")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    cpu_options = parse_options(args.cpu_with)
    gpu_options = None
    if args.cpu_only:
        validate_options(cpu_options, "vsag", args.k)
    else:
        gpu_options = parse_options(args.gpu_with)
        validate_comparable(cpu_options, gpu_options, args.k)
    environment = load_environment(args.environment_json)
    rows, dim, k = args.rows, args.dim, args.k
    m = int((gpu_options or cpu_options)["m"])
    vectors, queries, ids = generate_data(rows, dim, args.queries, args.seed, m, args.dataset)
    truth = exact_neighbors(vectors, queries, ids, k)
    run_id = uuid.uuid4().hex[:8]
    report = {
        "tool": "unittest/amd_hipvs/benchmark.py",
        "run_id": run_id,
        "started_at": datetime.now(timezone.utc).isoformat(),
        "config": {"rows": rows, "dim": dim, "queries": args.queries, "k": k,
                   "seed": args.seed, "dataset": args.dataset,
                   "repeats": args.repeats, "batch": args.batch,
                   "min_recall": args.min_recall, "target_speedup": args.target_speedup,
                   "cpu_only": args.cpu_only, "cpu_with": cpu_options,
                   "gpu_with": gpu_options,
                   "server_stderr": str(args.server_stderr) if args.server_stderr else None,
                   "coverage_samples": args.coverage_samples,
                   "skip_gpu_evidence": args.skip_gpu_evidence},
        "data": {"sha256": fingerprint(vectors, queries, ids), "rows": rows, "dim": dim,
                 "queries": args.queries, "k": k, "seed": args.seed,
                 "dataset": args.dataset},
        "server_environment": environment,
        "rounds": [],
        "active_operation": None,
    }
    connection = None
    try:
        connection = pymysql.connect(host=args.host, port=args.port, user=args.user,
                                     password=args.password, database=args.database,
                                     charset="utf8mb4", autocommit=False, connect_timeout=15)
        for number in range(1, args.repeats + 1):
            order = [("cpu", cpu_options)]
            if not args.cpu_only:
                order.append(("gpu", gpu_options))
            if number % 2 == 0:
                order.reverse()
            for mode, options in order:
                run_round(connection, report, mode, number, vectors, queries, ids, truth,
                          options)
        report["active_operation"] = None
        report["summary"] = summarize(report)
    except Exception as error:
        report["failure"] = f"{type(error).__name__}: {error}"
        traceback.print_exc()
        try:
            report["summary"] = summarize(report)
        except Exception as summary_error:
            report["summary"] = {"status": "INCOMPLETE", "speedup": None,
                                 "meets_target": False,
                                 "reasons": [f"summary failed: {summary_error}"]}
    finally:
        if connection is not None:
            connection.close()
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        save_report(report, args.output)
    status = report["summary"]["status"]
    print(json.dumps({"status": status, "speedup": report["summary"].get("speedup"),
                      "output": str(args.output)}, indent=2))
    return 0 if status in ("CPU_ONLY", "MEASURED_TARGET_UNSPECIFIED", "PASS") else 2


if __name__ == "__main__":
    sys.exit(main())
