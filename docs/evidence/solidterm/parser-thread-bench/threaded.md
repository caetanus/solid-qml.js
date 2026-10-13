### Throughput / CPU (median of runs; CPU = % of one core over the measured interval)

| term | case | runs | median s | p95 s | CPU total % | main % | render % | max load |
|---|---|---|---|---|---|---|---|---|
| solidterm | dense | 5 | 0.494 | 0.634 | 110 | 2 | 18 | 1.85 |
| solidterm | flood | 5 | 8.720 | 8.831 | 105 | 2 | 4 | 1.77 |
| solidterm | idle | 5 | 10.002 | 10.003 | 0 | 0 | 0 | 0.36 |
| solidterm | mixed | 5 | 3.424 | 3.464 | 109 | 2 | 8 | 1.66 |
| solidterm | region | 5 | 1.337 | 1.376 | 109 | 1 | 10 | 1.35 |
| solidterm | seq | 5 | 2.737 | 2.994 | 105 | 2 | 4 | 1.99 |

### Frames (frameSwapped intervals inside the measured interval)

| term | case | runs | fps | median ms | p95 ms | p99 ms | max ms | % intervals > 16.7 ms | updatePaintNode avg ms | max ms |
|---|---|---|---|---|---|---|---|---|---|---|
| solidterm | dense | 5 | 64.5 | 16.04 | 16.12 | 16.14 | 16.1 | 0.0 | 0.998 | 10.14 |
| solidterm | flood | 5 | 62.4 | 16.05 | 16.13 | 16.18 | 18.0 | 0.2 | 0.372 | 13.07 |
| solidterm | idle | 5 | 1.9 | 524.95 | 529.43 | 545.97 | 550.0 | 100.0 | 0.089 | 0.17 |
| solidterm | latency | 5 | 12.9 | 16.21 | 148.31 | 523.43 | 525.3 | 43.1 | 0.562 | 7.53 |
| solidterm | mixed | 5 | 62.6 | 16.05 | 16.14 | 16.19 | 17.8 | 0.5 | 0.836 | 77.87 |
| solidterm | region | 5 | 63.1 | 16.05 | 16.14 | 16.17 | 16.2 | 0.0 | 0.630 | 11.31 |
| solidterm | seq | 5 | 62.6 | 16.06 | 16.13 | 16.19 | 16.3 | 0.0 | 0.426 | 14.30 |
| solidterm | wheel | 5 | 59.7 | 16.20 | 16.43 | 16.49 | 363.9 | 0.5 | 0.144 | 12.71 |

### GUI-thread availability during the flood (5 ms timer: lateness per firing)

| term | case | runs | p50 ms | p95 ms | p99 ms | max ms | firings > 10 ms late | firings |
|---|---|---|---|---|---|---|---|---|
| solidterm | flood | 5 | 0.05 | 0.70 | 0.90 | 16.6 | 5 | 8615 |

### Input latency (ms since keyPressEvent, or since injection for `flood`; cumulative)

solidterm flood, 300 keys with every stage (of 300 sent):

| stage | median | p95 | max |
|---|---|---|---|
| keyevent | 0.06 | 0.17 | 0.85 |
| ptywrite | 0.20 | 0.46 | 1.12 |
| ptyread | 1.45 | 2.34 | 2.79 |
| damage | 1.67 | 2.68 | 3.17 |
| paint0 | 9.81 | 16.99 | 18.70 |
| paint1 | 10.03 | 17.32 | 19.08 |
| swap | 25.79 | 33.01 | 34.72 |
solidterm latency, 300 keys with every stage (of 300 sent):

| stage | median | p95 | max |
|---|---|---|---|
| ptywrite | 0.01 | 0.03 | 0.10 |
| ptyread | 0.17 | 0.24 | 0.43 |
| damage | 0.18 | 0.24 | 0.44 |
| paint0 | 0.78 | 1.01 | 2.49 |
| paint1 | 0.95 | 3.42 | 13.58 |
| swap | 3.67 | 4.87 | 14.06 |

Grid: solidterm 105x56

RHI/GPU (QSG_INFO):

    qt.rhi.general: OpenGL VENDOR: Intel RENDERER: Mesa Intel(R) UHD Graphics 630 (CFL GT2) VERSION: 4.6 (Compatibility Profile) Mesa 26.2.2-arch1.1
    qt.scenegraph.general: threaded render loop
