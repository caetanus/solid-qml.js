### Throughput / CPU (median of runs; CPU = % of one core over the measured interval)

| term | case | runs | median s | p95 s | CPU total % | main % | render % | max load |
|---|---|---|---|---|---|---|---|---|
| solidterm | dense | 5 | 0.549 | 0.784 | 104 | 88 | 16 | 1.30 |
| solidterm | flood | 5 | 8.928 | 9.133 | 102 | 98 | 3 | 1.69 |
| solidterm | idle | 5 | 10.002 | 10.002 | 0 | 0 | 0 | 0.43 |
| solidterm | mixed | 5 | 3.432 | 4.594 | 102 | 95 | 7 | 1.67 |
| solidterm | region | 5 | 1.477 | 1.525 | 104 | 95 | 8 | 1.02 |
| solidterm | seq | 5 | 2.946 | 3.082 | 101 | 97 | 4 | 1.47 |

### Frames (frameSwapped intervals inside the measured interval)

| term | case | runs | fps | median ms | p95 ms | p99 ms | max ms | % intervals > 16.7 ms | updatePaintNode avg ms | max ms |
|---|---|---|---|---|---|---|---|---|---|---|
| solidterm | dense | 5 | 64.2 | 16.04 | 16.12 | 16.15 | 16.2 | 0.0 | 0.786 | 10.09 |
| solidterm | flood | 5 | 62.4 | 16.06 | 16.14 | 16.22 | 18.0 | 0.2 | 0.149 | 7.69 |
| solidterm | idle | 5 | 1.9 | 524.95 | 525.85 | 525.87 | 525.9 | 100.0 | 0.085 | 0.16 |
| solidterm | latency | 5 | 12.7 | 16.17 | 147.95 | 523.09 | 525.3 | 43.5 | 0.539 | 7.44 |
| solidterm | mixed | 5 | 62.6 | 16.05 | 16.13 | 16.19 | 16.5 | 0.0 | 0.619 | 63.75 |
| solidterm | region | 5 | 63.0 | 16.05 | 16.14 | 16.17 | 16.2 | 0.0 | 0.487 | 8.77 |
| solidterm | seq | 5 | 62.6 | 16.05 | 16.14 | 16.20 | 16.3 | 0.0 | 0.174 | 7.69 |
| solidterm | wheel | 5 | 59.9 | 16.15 | 16.40 | 16.49 | 343.0 | 0.5 | 0.132 | 7.70 |

### GUI-thread availability during the flood (5 ms timer: lateness per firing)

| term | case | runs | p50 ms | p95 ms | p99 ms | max ms | firings > 10 ms late | firings |
|---|---|---|---|---|---|---|---|---|
| solidterm | flood | 5 | 12.03 | 13.08 | 13.75 | 22.3 | 2601 | 2613 |

### Input latency (ms since keyPressEvent, or since injection for `flood`; cumulative)

solidterm flood, 300 keys with every stage (of 300 sent):

| stage | median | p95 | max |
|---|---|---|---|
| keyevent | 3.78 | 8.07 | 8.83 |
| ptywrite | 3.78 | 8.08 | 8.84 |
| ptyread | 4.75 | 9.00 | 10.79 |
| damage | 5.86 | 10.07 | 11.68 |
| paint0 | 16.27 | 24.84 | 25.48 |
| paint1 | 16.38 | 24.94 | 25.59 |
| swap | 28.14 | 37.17 | 40.43 |
solidterm latency, 300 keys with every stage (of 300 sent):

| stage | median | p95 | max |
|---|---|---|---|
| ptywrite | 0.01 | 0.03 | 0.08 |
| ptyread | 0.25 | 0.37 | 0.58 |
| damage | 0.25 | 0.37 | 0.58 |
| paint0 | 0.75 | 0.91 | 1.22 |
| paint1 | 0.97 | 2.83 | 9.28 |
| swap | 3.64 | 4.19 | 9.57 |

Grid: solidterm 105x56

RHI/GPU (QSG_INFO):

    qt.rhi.general: OpenGL VENDOR: Intel RENDERER: Mesa Intel(R) UHD Graphics 630 (CFL GT2) VERSION: 4.6 (Compatibility Profile) Mesa 26.2.2-arch1.1
    qt.scenegraph.general: threaded render loop
