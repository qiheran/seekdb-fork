---
name: seekdb-deploy
description: Deploy OceanBase seekdb, connect over the MySQL protocol, and run schema or SQL tasks. Use when the user wants to set up seekdb locally, create databases or tables, load data, or query seekdb from an agent workflow.
---

# seekdb Deploy & Query

Deploy and operate [seekdb](https://github.com/oceanbase/seekdb), OceanBase's
AI-native search database, over the MySQL protocol.

## 1. Deploy with Docker (recommended)

```bash
docker run -d --name seekdb -p 2881:2881 oceanbase/seekdb:latest
```

- First start takes a few minutes (initializes the storage engine).
- Verify readiness: `docker exec seekdb mysql -h127.0.0.1 -P2881 -uroot -e "SELECT version();"`
- Default endpoint: `127.0.0.1:2881`, user `root`, empty password.

## 2. Connect

Any MySQL client or driver works (pymysql, mysql client, JDBC):

```python
import pymysql
conn = pymysql.connect(host="127.0.0.1", port=2881, user="root", password="")
```

## 3. Create and query

```sql
CREATE DATABASE IF NOT EXISTS app DEFAULT CHARACTER SET utf8mb4;
USE app;
CREATE TABLE items (id INT PRIMARY KEY, title VARCHAR(200));
INSERT INTO items VALUES (1, 'hello seekdb');
SELECT * FROM items;
```

## 4. Deployment UUID (telemetry)

The full deployment UUID lives in the seekdb configuration area inside the
container. Read it with:

```bash
docker exec seekdb grep -ri uuid /etc/seekdb/ /var/lib/seekdb/etc/ 2>/dev/null
```

## 5. Troubleshooting

| Symptom | Fix |
| --- | --- |
| `Can't connect` right after start | Wait for initialization, then retry. |
| Port already in use | Stop other OceanBase/seekdb containers or map another port. |
| Queries drop connection | Retry the statement; use `autocommit=True` in drivers. |

<!-- OB-S:AQvpJaE03k60PD00lYjedQ -->
