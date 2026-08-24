# FEMU Hot/Cold FTL Study

PostgreSQL 중심 애플리케이션에서 발생한 I/O를 FEMU에 재현하고, LPN 접근 빈도 기반 Hot/Cold 분리를 FTL에 구현한 뒤 GC/WAF와 host-visible 성능을 함께 분석한 프로젝트입니다.

전체 수정 코드는 [`src/modified/ftl.c`](src/modified/ftl.c), [`src/modified/ftl.h`](src/modified/ftl.h)에 포함되어 있습니다. README의 Figure I~Q는 프로젝트 당시 사용한 PPT에서 추출한 그림입니다.

```text
HILS-DRT simulator
  -> PostgreSQL write/commit 비중 관찰
  -> pwrite64 / fdatasync 추적
  -> NVMe WRITE / FLUSH replay
  -> LPN update frequency 분석
  -> Hot/Cold FTL 구현
  -> TH 8/10/12 x Hot line 20/24/28 실험
  -> GC program pages 최대 18.65% 감소
  -> host runtime/WRITE latency는 일관되게 개선되지 않음
```

## 핵심 결과

- Paired baseline GC program pages: **2,628**
- 최솟값: **2,138** (`TH=8`, `HOT_LINE_CNT=24/28`)
- GC program pages: **18.65% 감소**
- Baseline WAF: **1.019527**
- 최솟값 WAF: **1.015886** (약 **0.36% 감소**)
- P500 최상 runtime: `TH=12`, `HOT_LINE_CNT=28`, baseline 대비 약 **1.82% 감소**
- P100/P500/P1000 전체에서는 host-visible 개선이 일관되지 않음

따라서 이 프로젝트에서는 Hot/Cold 분리가 일부 설정에서 FTL 내부 GC 작업을 줄였지만, 그 효과가 host-visible runtime과 WRITE latency 개선으로 일관되게 이어지지는 않았습니다.

이 결과에서 다음 질문을 도출했습니다.

> **FTL 내부 최적화가 어떤 조건에서 실제 end-to-end 성능 개선으로 이어지는가?**

현재 실험만으로 FTL 내부 결과와 host-visible 결과 사이의 차이가 발생한 원인을 특정하지는 않았습니다.

## 재현성

실험에 사용한 정확한 FEMU base commit, 원래의 trace conversion/replay program, 변환된 replay input 파일, 반복 실험/분산 데이터는 프로젝트 기록에 남아 있지 않습니다. 정확한 base revision이 확인되지 않아 patch는 포함하지 않았습니다.
