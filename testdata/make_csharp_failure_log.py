"""Produces testdata/csharp-failure.log from real runs of the csharp-orders sample (needs the .NET SDK).

Run 1 uses appsettings.json as shipped: Orders:BatchSize is -5, so the program stops with an unhandled
ArgumentOutOfRangeException. Run 2 sets ORDERS_BATCH_SIZE=25 and then fails to find the order file
under Orders:DataDir, retries MaxRetries times and gives up.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "csharp-orders")
LOG = os.path.join(HERE, "csharp-failure.log")


def main() -> int:
    with tempfile.TemporaryDirectory() as out:
        # Build output goes to a temporary folder so the sample folder holds only source.
        subprocess.run(["dotnet", "build", APP, "-c", "Debug", "-o", os.path.join(out, "bin"),
                        "-p:BaseIntermediateOutputPath=" + os.path.join(out, "obj") + os.sep, "--nologo", "-v", "q"],
                       check=True)
        exe = os.path.join(out, "bin", "Orders.dll")
        env = dict(os.environ)
        runs = [
            (["dotnet", exe, "--region", "west"], {}),
            (["dotnet", exe, "--region", "west"], {"ORDERS_BATCH_SIZE": "25"}),
        ]
        with open(LOG, "w", encoding="utf-8", newline="\n") as log:
            for args, extra in runs:
                result = subprocess.run(args, capture_output=True, text=True, env={**env, **extra})
                log.write(result.stderr.replace("\r\n", "\n"))

    print(LOG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
