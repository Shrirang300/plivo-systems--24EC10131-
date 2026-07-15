| Delay (ms) | Miss Rate (%) | Overhead (x) | Status | Notes |
|------------|---------------|--------------|--------|-------|
| 250        | < 1.0%        | 1.6x         | VALID  | Baseline established. |
| 200        | 0.47%         | 1.6x         | VALID  | Stable performance. |
| 170        | 1.20%         | 1.6x         | INVALID| Approaching the limit. |
| 160        | 3.93%         | 1.77x        | INVALID| Burst loss threshold reached. |
| 150        | 4.00%         | 1.76x        | INVALID| Jitter noise. |
| 140        | 3.80%         | 1.77x        | INVALID| Unstable. |
| 80         | 100%          | 1.77x        | INVALID| Receiver starvation. |
