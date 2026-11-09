# AMD hipVS 端到端构建验证工具

本目录只驱动**真实的 seekdb SQL 服务**和**真实的 AMD GPU**。没有 GPU 模拟器，`lib=hipvs`
在缺少桥接库或 GPU 时直接失败，不会静默回退到 CPU；CPU 回退也不会被计作 GPU 构建成绩。
在 AMD 主机上完成实测并把报告提交之前，本目录只是工具，不代表题目已通过。

## 适用范围

当前 `lib=hipvs` 后端支持且仅支持：

- `type=hnsw`、`distance=l2`、FP32 向量；
- 无 extra info；当前实测范围是非分区表；
- 行数 `rows > 2*m`，`5 <= m <= 128`，`ef_construction > m`。

其他组合会返回 `OB_NOT_SUPPORTED`，这是有意为之，避免用不匹配的配置产生虚假成绩。

## 前置条件

1. seekdb 以 `-DSEEKDB_WITH_HIPVS=ON` 构建，且 `libseekdb_hipvs_build.so` 与 seekdb
   可执行文件位于同一目录，或放在 `<exe>/../lib/seekdb/`，或显式设置
   `SEEKDB_HIPVS_BUILD_LIBRARY`（必须是绝对路径）。
2. 服务进程所在主机安装 ROCm/hipVS，且能看到 AMD GPU。用
   `python probe_gpu.py --library <bridge.so>` 先确认桥接库可用。
3. Python 3.10+。安装依赖：

   ```bash
   python3 -m venv .venv
   . .venv/bin/activate
   python -m pip install -r requirements.txt
   ```

AMD 云实例上的典型流程：

```bash
./build.sh release --init -DSEEKDB_WITH_HIPVS=ON --make -j8
mkdir -p ~/seekdb/bin
cp build_release/src/observer/seekdb build_release/src/observer/libseekdb_hipvs_build.so ~/seekdb/bin
ldd ~/seekdb/bin/libseekdb_hipvs_build.so   # 应能找到 libcuvs_c、libamdhip64 等
cd ~/seekdb && ./bin/seekdb                 # 另开终端运行基准；默认端口 2881
```

4. 数据库账号需要在专用测试库中建表、建/删向量索引的权限。工具每轮创建带随机 run ID
   的新表，只删除自己成功创建的表；不会删除整个数据库。请使用**专用实例**，运行期间
   不要有其他建索引操作，否则 GPU 证据区间会不唯一。

连接参数来自环境变量，避免把密码写进命令历史：

| 变量 | 默认值 |
|---|---|
| `AMD_BENCH_HOST` | `127.0.0.1` |
| `AMD_BENCH_PORT` | `2881` |
| `AMD_BENCH_USER` | `root` |
| `AMD_BENCH_PASSWORD` | 空 |
| `AMD_BENCH_DATABASE` | `test` |

## 导入校验器 CPU 自测

`validate_stream_test.cpp` 只依赖 `src/oblib` 头文件，不需要 GPU、ROCm 或 seekdb
构建，用合成 HNSW 流验证标签重映射与拒绝路径（头字段、步长、度数/邻居越界、
标签重复、向量/标签不一致、截断、尾随数据、VID 重复）：

```bash
bash unittest/amd_hipvs/run_import_test.sh   # 或 CXX=clang++ bash ...
```

## 桥接库自检

在 AMD 实例上运行，不连接数据库，验证 GPU 确实完成构图，并逐字节检查导出的
HNSW 流（头部、层 0 度数/邻居、FP32 向量与标签对应、标签唯一、上层结构、无尾随数据）：

```bash
python probe_gpu.py --library /workspace/seekdb-build/libseekdb_hipvs_build.so \
  --output-dir local-results --threads 8
```

输出 `local-results/gpu-probe.json`。该检查不测 SQL 端到端耗时，不能替代 `benchmark.py`。

## CPU 功能冒烟

小规模参数只用于确认 SQL 语法与索引可用，**不是赛事评分规模**：

```bash
python benchmark.py --cpu-only --rows 200 --dim 16 --queries 20 --k 10 \
  --repeats 3 --min-recall 0.90 --output local-results/cpu-smoke.json
```

结果状态应为 `CPU_ONLY`。默认 CPU 参数为
`distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200`。
查询使用 `/*+ INDEX(t idx) */` 提示和 `APPROXIMATE LIMIT`，并用 `EXPLAIN` 确认计划确实
引用了指定向量索引。

## CPU/GPU 对比

```bash
python benchmark.py --rows 10000 --dim 128 --queries 100 --k 10 --repeats 3 \
  --cpu-with "distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200" \
  --gpu-with "distance=l2,type=hnsw,lib=hipvs,m=16,ef_construction=200,ef_search=200" \
  --min-recall 0.90 \
  --environment-json environment.json \
  --server-stderr /proc/$(pgrep -n seekdb)/fd/2 \
  --output local-results/benchmark-10000x128.json
```

`--cpu-with` / `--gpu-with` 必须是完整的 `WITH (...)` 参数；CPU 必须是 `lib=vsag`，
GPU 必须是 `lib=hipvs`，两边 `distance/type/m/ef_construction/ef_search` 必须完全一致。
`--target-speedup` 由操作者按统一评测配置填写；不填时状态为
`MEASURED_TARGET_UNSPECIFIED`，工具不会声称达到任何阈值。

### 计时口径

每轮新建空表并导入完全相同的 FP32 向量和互不相同的大整数主键（高于 2^53，便于发现
ID 精度丢失）。导入不计时。计时区间从 `CREATE VECTOR INDEX` 开始，到第一条使用指定
索引的 ANN 查询成功返回为止，包含扫描、GPU 传输/同步、格式转换、DDL 和可能的首次
延迟构建。数据生成、导入、Recall 校验与自查询诊断不计入构建耗时。

召回真值用同一份 FP32 数据的 FP64 距离累加精确计算，按 `距离, id` 稳定排序取前 K。
`Recall@K` 使用每轮相同的查询集与 `ob_hnsw_ef_search`（会话变量会覆盖 `WITH` 中的
`ef_search`，脚本会显式设置并回读确认）。

### 数据分布

`--dataset` 决定生成什么数据，CPU 与 GPU 轮次始终使用同一份：

- `gaussian`（默认）：i.i.d. 标准正态，是 ANN 的最坏情况。维度一高所有两两距离都集中，
  HNSW 的召回会随行数显著下降，且与构建后端无关（10K 规模 Recall@10 约 0.96，1M 规模
  即使把 `ef_search` 提到 6400 也只有约 0.59）。
- `clustered`：围绕 `max(16, rows/1024)` 个高斯中心生成点与查询，更接近真实 embedding
  的簇结构，用于在较大规模下比较两条路径的构建速度和召回。

报告在 `config.dataset` 与 `data.dataset` 中记录实际使用的分布；两种分布的结果不可
互相比较，也不可只报告对自己有利的一种。

CPU/GPU 各跑 `--repeats` 轮，顺序逐轮交替，加速比取**两条路径构建耗时的中位数之比**，
不是每轮加速比的平均。任一未完成、Recall 不达标或查询计划未验证时，不给加速成绩。

### GPU 证据

桥接库在真实构图成功后会向 seekdb 进程的 stderr 写一行：

```
[seekdb-hipvs] backend=hipvs vendor=AMD device="<GPU 型号>" arch=gfxXXXX rows=<n> dim=<d> m=<m> ef_construction=<e> gpu_compute=true cpu_fallback=false
```

`--server-stderr` 指向该进程的 stderr（OceanBase 通常重定向到数据目录的
`log/seekdb.log`；以 `/proc/<pid>/fd/2` 为准）。脚本只读取**本轮建索引前到首查询完成后
新追加**的字节区间，要求恰好一条记录、行数/维度/M/ef_construction 与 SQL 输入一致，
并保存区间字节数与 SHA256。日志轮转、区间过大、记录缺失或重复都会使该轮证据失败。

`--skip-gpu-evidence` 仅用于快速冒烟；此时 GPU 对比状态为 `UNVERIFIED`，不产生有效
加速比。

### 服务器环境

`--environment-json` 必须描述**服务器**（不是客户端笔记本），至少包含：

```json
{
  "cpu_model": "...",
  "cpu_threads": 32,
  "cpu_affinity": "0-31",
  "gpu_model": "AMD Instinct MI300X",
  "gpu_architecture": "gfx942",
  "rocm_version": "6.x",
  "hipvs_version": "...",
  "seekdb_commit": "<完整 commit>"
}
```

脚本只读取上述字段，不保存密码、SSH 地址或令牌。GPU 模式下缺少环境字段或 GPU 证据时，
状态为 `UNVERIFIED`，加速比为 `null`。

## 大规格复现

赛事主机内存足够时可用 1M×96 规模（建议 `--batch 256`，预留至少 8 GiB 内存给数据库与
索引构建，另需本地保存原始向量以计算精确真值）：

```bash
python benchmark.py --rows 1000000 --dim 96 --queries 100 --k 10 --repeats 3 \
  --batch 256 --min-recall 0.90 \
  --environment-json environment.json \
  --server-stderr /proc/$(pgrep -n seekdb)/fd/2 \
  --output local-results/benchmark-1000000x96.json
```

导入为逐批多行 INSERT，数据生成与导入不计时，但会占用数分钟；每轮都会重建表，请评估
磁盘与时间预算。`--coverage-samples N` 可额外抽样做“输入向量查询自身”的可达性诊断，
只作参考，不计入构建耗时，也不代替 Recall 门槛。

## 状态与退出码

| 状态 | 含义 | 退出码 |
|---|---|---|
| `CPU_ONLY` | CPU 基线完成 | 0 |
| `MEASURED_TARGET_UNSPECIFIED` | CPU/GPU 均已测量，未提供性能目标 | 0 |
| `PASS` | 达到操作者给定的 Recall 与加速比目标 | 0 |
| `PERFORMANCE_TARGET_NOT_MET` | 已测量但未达到给定加速比 | 2 |
| `UNVERIFIED` | 缺少 GPU 证据或服务器环境，不计算加速比 | 2 |
| `QUALITY_OR_FUNCTION_FAILED` | Recall 不达标或查询计划未使用指定索引 | 2 |
| `INCOMPLETE` | 轮次未完成或运行出错 | 2 |

报告同时写出 `--output` 指定的 JSON 与同名 `.md`。失败时仍保存已完成轮次和错误信息；
发生异常后数据表会尽量删除，残留表名在报告中可查，请按准确表名人工清理。

## 诚实性约束

- 不允许手写、改写或拼接 GPU 证据；证据必须来自被测 seekdb 构建过程的原始 stderr。
- 不允许用 CPU 索引或 CPU 查询耗时冒充 GPU 构建成绩。
- 不允许缩小数据、降低维度或使用更低精度来宣称满足统一评测。
- 本工具不验证数据库重启恢复、内存泄漏或 GPU 性能分析；这些需要额外证据。
- 竞赛结论以平台统一评测为准，本工具的输出只是可复现的本地证据。
