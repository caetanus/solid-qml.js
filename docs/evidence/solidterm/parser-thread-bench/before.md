### Throughput / CPU (median of runs; CPU = % of one core over the measured interval)

| term | case | runs | median s | p95 s | CPU total % | main % | render % | max load |
|---|---|---|---|---|---|---|---|---|
| solidterm | dense | 5 | 0.540 | 0.553 | 102 | 87 | 14 | 1.05 |
| solidterm | mixed | 5 | 3.661 | 5.720 | 102 | 95 | 7 | 1.76 |
| solidterm | region | 5 | 1.466 | 1.562 | 104 | 95 | 8 | 1.06 |
| solidterm | seq | 5 | 2.816 | 2.911 | 102 | 98 | 4 | 1.40 |

### Frames (frameSwapped intervals inside the measured interval)

| term | case | runs | fps | median ms | p95 ms | p99 ms | max ms | % intervals > 16.7 ms | updatePaintNode avg ms | max ms |
|---|---|---|---|---|---|---|---|---|---|---|
| solidterm | dense | 5 | 64.2 | 16.04 | 16.12 | 16.13 | 16.1 | 0.0 | 0.915 | 8.53 |
| solidterm | latency | 5 | 12.9 | 16.15 | 147.98 | 523.20 | 525.0 | 43.5 | 0.539 | 7.66 |
| solidterm | mixed | 5 | 62.6 | 16.05 | 16.12 | 16.19 | 16.5 | 0.0 | 0.567 | 65.31 |
| solidterm | region | 5 | 63.0 | 16.04 | 16.12 | 16.14 | 16.1 | 0.0 | 0.455 | 8.91 |
| solidterm | seq | 5 | 62.6 | 16.06 | 16.15 | 16.24 | 16.4 | 0.0 | 0.148 | 7.58 |
| solidterm | wheel | 5 | 60.0 | 16.17 | 16.41 | 16.48 | 332.9 | 0.5 | 0.110 | 9.41 |

### Input latency (ms since keyPressEvent, or since injection for `flood`; cumulative)

solidterm latency, 300 keys with every stage (of 300 sent):

| stage | median | p95 | max |
|---|---|---|---|
| ptywrite | 0.01 | 0.02 | 0.05 |
| ptyread | 0.26 | 0.37 | 0.92 |
| damage | 0.26 | 0.37 | 0.92 |
| paint0 | 0.76 | 0.91 | 1.36 |
| paint1 | 0.94 | 3.27 | 9.15 |
| swap | 3.68 | 4.37 | 9.43 |

Grid: solidterm 105x56

RHI/GPU (QSG_INFO):

    qt.rhi.general: OpenGL VENDOR: Intel RENDERER: Mesa Intel(R) UHD Graphics 630 (CFL GT2) VERSION: 4.6 (Compatibility Profile) Mesa 26.2.2-arch1.1
    qt.scenegraph.general: threaded render loop
