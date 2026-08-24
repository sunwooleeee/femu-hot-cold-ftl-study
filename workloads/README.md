# Workloads

The experiment replays storage operations derived from the PostgreSQL path of the HILS-DRT simulator.

| Workload | WRITE | FLUSH | Total operations | Total bytes |
|---|---:|---:|---:|---:|
| P100 | 4,841 | 4,686 | 9,527 | 41,639,936 |
| P500 | 21,800 | 20,609 | 42,409 | 187,506,688 |
| P1000 | 37,335 | 34,304 | 71,639 | 322,093,056 |

Approximately 95% of WRITE requests were 8 KB, and the workload frequently issued a write followed by a flush.


