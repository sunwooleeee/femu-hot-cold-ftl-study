# FEMU Hot/Cold FTL Study

PostgreSQL 중심 애플리케이션에서 발생한 I/O를 FEMU에 재현하고, LPN 접근 빈도 기반 Hot/Cold 분리를 FTL에 구현하여 GC/WAF와 Host-visible 성능을 함께 분석한 프로젝트입니다.

전체 수정 코드는 [`src/modified/ftl.c`](src/modified/ftl.c), [`src/modified/ftl.h`](src/modified/ftl.h)에 포함되어 있습니다. README의 Figure I~Q는 별도로 다시 그린 그림이 아니라 **프로젝트 당시 사용한 PPT에 삽입되어 있던 Figure 원본을 그대로 추출한 것**입니다.

핵심 흐름은 다음과 같습니다.

```text
HILS-DRT simulator
  -> PostgreSQL write/commit 비중 관찰
  -> pwrite64 / fdatasync 추적
  -> NVMe WRITE / FLUSH replay
  -> LPN update frequency 분석
  -> Hot/Cold FTL 구현
  -> TH 8/10/12 x Hot line 20/24/28 실험
  -> GC program pages 최대 18.65% 감소
  -> 그러나 host runtime/WRITE latency는 일관되게 개선되지 않음
```

## 핵심 결과

- Paired baseline GC program pages: **2,628**
- 최솟값: **2,138** (`TH=8`, `HOT_LINE_CNT=24/28`)
- GC copy/program work: **18.65% 감소**
- Baseline WAF: **1.019527**
- 최솟값 WAF: **1.015886** (약 **0.36% 감소**)
- P500 최상 runtime: `TH=12`, `HOT_LINE_CNT=28`, baseline 대비 약 **1.82% 감소**
- P100/P1000까지 포함하면 host-visible 개선은 일관되지 않음

따라서 이 프로젝트의 결론은 단순히 “Hot/Cold 분리가 성능을 개선한다”가 아닙니다. **FTL 내부 GC 효율 개선이 언제 실제 end-to-end 성능 개선으로 이어지는가**라는 후속 시스템 질문을 도출한 것이 핵심입니다.

## 재현성 주의

정확한 과거 FEMU base commit과 원래 사용한 trace conversion/replay program은 제공된 실험 자료에 남아 있지 않습니다. 따라서 `scripts/`의 파일들은 문서화된 실험 절차를 다시 실행하기 위한 보조 wrapper이며, 당시 사용한 원본 스크립트라고 주장하지 않습니다. Patch 역시 확인되지 않은 base를 임의로 정해 생성하지 않았으며 자세한 내용은 [`patches/README.md`](patches/README.md)에 기록했습니다.
