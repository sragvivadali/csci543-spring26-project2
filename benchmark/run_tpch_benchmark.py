#!/usr/bin/env python3
"""
TPC-H JIT vs. Vectorization Benchmark  (CSCI 543 – Project 2)
==============================================================

Runs all 22 TPC-H queries twice:
  1. Vectorized  – DuckDB's native vectorized engine  (SET jit_enabled = false)
  2. JIT         – Your LLVM JIT extension            (SET jit_enabled = true)

Prerequisites
-------------
  pip install duckdb matplotlib pandas

  *OR* build your custom DuckDB Python wheel:
  cd duckdb && BUILD_PYTHON=1 make debug

Usage
-----
  # Quick test  (SF 0.1, 3 iterations)
  python3 run_tpch_benchmark.py

  # Full benchmark (SF 1, 5 iterations)
  python3 run_tpch_benchmark.py --sf 1 --iters 5 --out results/sf1.csv

  # Plot results after running
  python3 run_tpch_benchmark.py --plot results/sf1.csv
"""

import argparse
import csv
import os
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths – adjust if you move this script
# ---------------------------------------------------------------------------
SCRIPT_DIR   = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent
QUERY_DIR    = PROJECT_ROOT / "duckdb" / "extension" / "tpch" / "dbgen" / "queries"

# ---------------------------------------------------------------------------
# Try to import duckdb (stock or custom build)
# ---------------------------------------------------------------------------
try:
    import duckdb
except ImportError:
    sys.exit(
        "ERROR: duckdb Python package not found.\n"
        "  Install: pip install duckdb\n"
        "  Or build your custom wheel: cd duckdb && BUILD_PYTHON=1 make debug"
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_query(q: int) -> str:
    """Read TPC-H query q (1-22) from the dbgen directory."""
    path = QUERY_DIR / f"q{q:02d}.sql"
    if not path.exists():
        raise FileNotFoundError(f"Query file not found: {path}")
    return path.read_text()


def bench_query(con, sql: str, iterations: int) -> float:
    """Run `sql` `iterations` times (after 1 warm-up) and return avg wall ms."""
    # warm-up
    try:
        con.execute(sql).fetchall()
    except Exception:
        pass

    times = []
    for _ in range(iterations):
        t0 = time.perf_counter()
        try:
            con.execute(sql).fetchall()
        except Exception as e:
            print(f"    [WARN] {e}")
            return -1.0
        times.append((time.perf_counter() - t0) * 1000.0)  # ms

    return sum(times) / len(times)


def set_jit(con, enabled: bool) -> None:
    """Toggle JIT via the pragma your extension registers."""
    val = "true" if enabled else "false"
    try:
        con.execute(f"SET jit_enabled = {val}")
    except Exception:
        # Pragma may not exist in vanilla duckdb – silently skip
        pass


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run_benchmark(sf: float, iterations: int, csv_out: str) -> list:
    print(f"\n{'='*60}")
    print(f"  TPC-H JIT vs. Vectorization  (SF={sf}, iters={iterations})")
    print(f"{'='*60}\n")

    con = duckdb.connect()

    # Load TPC-H extension (built-in since DuckDB 0.6+)
    try:
        con.execute("INSTALL tpch")
        con.execute("LOAD tpch")
    except Exception as e:
        print(f"[INFO] Could not install/load tpch extension: {e}")
        print("       Trying to use it as a built-in …")

    # Generate data
    print(f"Generating TPC-H data at SF{sf} …")
    con.execute(f"CALL dbgen(sf={sf})")
    print("Done.\n")

    # Table header
    print(f"{'Query':<8} {'Vec (ms)':>12} {'JIT (ms)':>12} {'Speedup':>10}")
    print("-" * 44)

    results = []
    for q in range(1, 23):
        qname = f"Q{q:02d}"
        try:
            sql = load_query(q)
        except FileNotFoundError as e:
            print(f"{qname:<8}  [SKIPPED – {e}]")
            continue

        # --- Vectorized (JIT off) ---
        set_jit(con, False)
        vec_ms = bench_query(con, sql, iterations)

        # --- JIT (JIT on) ---
        set_jit(con, True)
        jit_ms = bench_query(con, sql, iterations)

        speedup = (vec_ms / jit_ms) if jit_ms > 0 else 0.0
        jit_err = jit_ms < 0

        tag = "  [JIT ERROR]" if jit_err else ""
        print(f"{qname:<8} {vec_ms:>12.2f} {jit_ms:>12.2f} {speedup:>10.2f}x{tag}")

        results.append({
            "query":         qname,
            "vectorized_ms": round(vec_ms, 3),
            "jit_ms":        round(jit_ms, 3),
            "speedup":       round(speedup, 4),
            "jit_error":     jit_err,
        })

    # Summary
    valid = [r for r in results if not r["jit_error"]]
    if valid:
        total_vec = sum(r["vectorized_ms"] for r in valid)
        total_jit = sum(r["jit_ms"] for r in valid)
        overall   = total_vec / total_jit if total_jit > 0 else 0
        faster    = sum(1 for r in valid if r["speedup"] > 1.05)
        slower    = sum(1 for r in valid if r["speedup"] < 0.95)

        print(f"\n{'='*44}")
        print(f"  Total vectorized : {total_vec:.1f} ms")
        print(f"  Total JIT        : {total_jit:.1f} ms")
        print(f"  Overall speedup  : {overall:.2f}x")
        print(f"  JIT faster (>5%) : {faster}/{len(valid)} queries")
        print(f"  JIT slower (<-5%): {slower}/{len(valid)} queries")

    # Write CSV
    os.makedirs(os.path.dirname(os.path.abspath(csv_out)), exist_ok=True)
    with open(csv_out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["query","vectorized_ms","jit_ms","speedup","jit_error"])
        w.writeheader()
        w.writerows(results)
    print(f"\nResults written to: {csv_out}")
    return results


def plot_results(csv_path: str) -> None:
    try:
        import pandas as pd
        import matplotlib.pyplot as plt
        import matplotlib.patches as mpatches
    except ImportError:
        sys.exit("ERROR: pip install pandas matplotlib")

    df = pd.read_csv(csv_path)
    df = df[df["jit_error"] == False].copy()

    fig, axes = plt.subplots(1, 2, figsize=(16, 6))
    fig.suptitle("TPC-H: JIT vs. Vectorized Execution (CSCI 543)", fontsize=14, fontweight="bold")

    # ---- Left: grouped bar chart (execution time) ----
    ax = axes[0]
    x = range(len(df))
    w = 0.35
    bars_vec = ax.bar([i - w/2 for i in x], df["vectorized_ms"], w, label="Vectorized", color="#4878CF")
    bars_jit = ax.bar([i + w/2 for i in x], df["jit_ms"],        w, label="JIT",        color="#D65F5F")
    ax.set_xlabel("TPC-H Query")
    ax.set_ylabel("Execution time (ms, lower is better)")
    ax.set_title("Execution Time per Query")
    ax.set_xticks(list(x))
    ax.set_xticklabels(df["query"].tolist(), rotation=45, ha="right", fontsize=8)
    ax.legend()
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    # ---- Right: speedup chart ----
    ax2 = axes[1]
    colors = ["#2ecc71" if s > 1.05 else "#e74c3c" if s < 0.95 else "#95a5a6"
              for s in df["speedup"]]
    ax2.bar(list(x), df["speedup"], color=colors)
    ax2.axhline(1.0, color="black", linestyle="--", linewidth=1.0, label="Baseline (1×)")
    ax2.set_xlabel("TPC-H Query")
    ax2.set_ylabel("Speedup  (JIT / Vectorized,  >1 = JIT faster)")
    ax2.set_title("JIT Speedup Over Vectorized Execution")
    ax2.set_xticks(list(x))
    ax2.set_xticklabels(df["query"].tolist(), rotation=45, ha="right", fontsize=8)
    ax2.legend(handles=[
        mpatches.Patch(color="#2ecc71", label="JIT faster (>5%)"),
        mpatches.Patch(color="#e74c3c", label="JIT slower (<-5%)"),
        mpatches.Patch(color="#95a5a6", label="Roughly equal"),
    ])
    ax2.grid(axis="y", linestyle="--", alpha=0.5)

    plt.tight_layout()
    out_png = csv_path.replace(".csv", "_plot.png")
    plt.savefig(out_png, dpi=150)
    print(f"Plot saved to: {out_png}")
    plt.show()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="TPC-H JIT vs Vectorization benchmark")
    parser.add_argument("--sf",    type=float, default=0.1,
                        help="TPC-H scale factor (default 0.1)")
    parser.add_argument("--iters", type=int,   default=3,
                        help="Measurement iterations per query (default 3)")
    parser.add_argument("--out",   type=str,   default="results/tpch_benchmark.csv",
                        help="Output CSV path")
    parser.add_argument("--plot",  type=str,   default=None,
                        help="Plot an existing CSV result file instead of running")
    args = parser.parse_args()

    if args.plot:
        plot_results(args.plot)
    else:
        run_benchmark(args.sf, args.iters, args.out)
