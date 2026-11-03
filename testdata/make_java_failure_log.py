"""Produces testdata/java-failure.log from real runs of the java-inventory sample (needs a JDK).

Run 1 uses config/inventory.properties as shipped: page.size = fifty, so the program stops with an
uncaught NumberFormatException. Run 2 overrides it with -Dinventory.pageSize=25 and then fails to find
the stock file under data.dir, retries MAX_RETRIES times and gives up.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "java-inventory")
LOG = os.path.join(HERE, "java-failure.log")
LOG_FORMAT = "-Djava.util.logging.SimpleFormatter.format=%1$tF %1$tT.%1$tL %4$-7s %3$s: %5$s%6$s%n"


def main() -> int:
    sources = []
    for root, _, files in os.walk(os.path.join(APP, "src")):
        sources += [os.path.join(root, f) for f in files if f.endswith(".java")]

    with tempfile.TemporaryDirectory() as classes:
        subprocess.run(["javac", "-d", classes] + sources, check=True)
        runs = [
            ["java", LOG_FORMAT, "-cp", classes, "com.example.inventory.InventoryApp", "--warehouse", "north"],
            ["java", LOG_FORMAT, "-Dinventory.pageSize=25", "-cp", classes, "com.example.inventory.InventoryApp",
             "--warehouse", "north"],
        ]
        with open(LOG, "w", encoding="utf-8", newline="\n") as log:
            for args in runs:
                result = subprocess.run(args, cwd=APP, capture_output=True, text=True)
                log.write(result.stderr)

    print(LOG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
