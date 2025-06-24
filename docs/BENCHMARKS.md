# Benchmarking PLJS vs PLV8

PLV8 uses Google's V8 JavaScript engine, whereas PLJS uses the QuickJS engine. QuickJS's performance relative to V8 is well-documented [here](https://bellard.org/quickjs/bench.html).

Since Postgres stored procedures are typically short-lived, which primarily serve data transformations, we focus instead on specific metrics such as type conversions between PostgreSQL and JavaScript, and startup times.

## Benchmarks

The benchmarks are currently separated into two sections: type conversion, and general performance; type conversions are general PostgreSQL to JavaScript and back type conversions.

### Conversion

Benchmarking conversions involves executing an `SPI` `SELECT` of the value, which converts from JavaScript to Postgres, and back.

| Test                    | Count  | Description             |
| ----------------------- | ------ | ----------------------- |
| JSON Conversion Small   | 10000  | Small JSON conversion   |
| JSON Conversion Medium  | 10000  | Medium JSON conversion  |
| JSONB Conversion Small  | 10000  | Small JSONB conversion  |
| JSONB Conversion Medium | 10000  | Medium JSONB conversion |
| INT Conversion          | 100000 | Integer conversion      |
| FLOAT Conversion        | 100000 | Float conversion        |
| TEXT Conversion         | 100000 | String conversion       |
| TIMESTAMP Conversion    | 100000 | Date conversion         |

### Performance

Performance depends on two specific benchmarks: engine startup and a simple array search test. The array search test provides insight into variable performance, and startup is critical for initializing the extension and executing stored procedures quickly.

It should be noted that once a context is created, it is active until the Postgres process is disconnected.

| Test                      | Count | Description                                 |
| ------------------------- | ----- | ------------------------------------------- |
| Array Creation and Search | 10000 | Create an array and search it               |
| Context Creation          | 1000  | Create a new JavaScript context and execute |

## Tested Platforms

Tested platforms were against ARM64/AARCH64 running both MacOS and Linux, and x86_64 running Linux. This was meant to show the differences both on an operating system level as well as a CPU level.

| CPU              | Operating System | Platform |
| ---------------- | ---------------- | -------- |
| M1 Max           | MacOS            | Aarch64  |
| AMD64            | Linux            | x86_64   |
| Rockchip RK3588S | Linux            | Arm64    |

## Results

The results show variation based on operating system and CPU.

### AARCH64 - MacOS

| Benchmark                         | PLJS       | PLV8       |
| --------------------------------- | ---------- | ---------- |
| JSON conversion small (10000)     | 219.416ms  | 165.447ms  |
| JSON conversion medium (10000)    | 4038.368ms | 553.211ms  |
| JSONB conversion small (10000)    | 219.808ms  | 137.069ms  |
| JSONB conversion medium (10000)   | 2740.983ms | 1726.405ms |
| INT conversion (100000)           | 1217.895ms | 1083.894ms |
| FLOAT conversion (100000)         | 1010.408ms | 966.111ms  |
| TEXT conversion (100000)          | 1059.067ms | 1070.629ms |
| TIMESTAMP conversion (100000)     | 1256.939ms | 1067.198ms |
| Array creation and search (10000) | 6628.59ms  | 4409.779ms |
| Context creation (1000)           | 9401.116ms | 2376.421ms |

### Arm64 - Linux

| Benchmark                         | PLJS        | PLV8       |
| --------------------------------- | ----------- | ---------- |
| JSON conversion small (10000)     | 319.792ms   | 403.188ms  |
| JSON conversion medium (10000)    | 2770.112ms  | 1197.231ms |
| JSONB conversion small (10000)    | 217.58ms    | 290.721ms  |
| JSONB conversion medium (10000)   | 1960.291ms  | 2817.583ms |
| INT conversion (100000)           | 2029.335ms  | 2682.387ms |
| FLOAT conversion (100000)         | 1781.802ms  | 2049.39ms  |
| TEXT conversion (100000)          | 1690.534ms  | 2229.296ms |
| TIMESTAMP conversion (100000)     | 1774.391ms  | 2226.814ms |
| Array creation and search (10000) | 17608.051ms | 9060.09ms  |
| Context creation (1000)           | 27881.981ms | 4864.181ms |

### x64_64 - Linux

| Benchmark                         | PLJS        | PLV8       |
| --------------------------------- | ----------- | ---------- |
| JSON conversion small (10000)     | 380.352ms   | 563.658ms  |
| JSON conversion medium (10000)    | 2551.5ms    | 1215.062ms |
| JSONB conversion small (10000)    | 249.23ms    | 415.243ms  |
| JSONB conversion medium (10000)   | 1920.07ms   | 3193.685ms |
| INT conversion (100000)           | 3489.673ms  | 3412.874ms |
| FLOAT conversion (100000)         | 2292.054ms  | 2819.443ms |
| TEXT conversion (100000)          | 1919.394ms  | 3059.18ms  |
| TIMESTAMP conversion (100000)     | 1926.551ms  | 3376.927ms |
| Array creation and search (10000) | 13970.188ms | 6276.774ms |
| Context creation (1000)           | 23741.54ms  | 5037.147ms |

```

```
