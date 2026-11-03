"""Produces testdata/scala-failure.log from real runs of the scala-shipping sample.

Needs Java 17, a Scala 3 distribution and four jars from Maven Central:
    python make_scala_failure_log.py <scala3 folder (with bin/ and lib/)> <folder with the jars>
    jars: config-1.4.3, slf4j-api-2.0.13, logback-classic-1.5.6, logback-core-1.5.6

Run 1 uses application.conf as shipped: shipping.max-parcels = 0, so the program stops with an uncaught
IllegalArgumentException. Run 2 overrides it with -Dshipping.max-parcels=50 and then fails to find the
parcel file under shipping.data-dir, retries MaxRetries times and gives up.
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "scala-shipping")
LOG = os.path.join(HERE, "scala-failure.log")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    scala_home, libs = sys.argv[1], sys.argv[2]
    jars = sorted(glob.glob(os.path.join(libs, "*.jar")))
    scala_lib = [j for j in glob.glob(os.path.join(scala_home, "lib", "*.jar"))
                 if os.path.basename(j).startswith(("scala3-library_3", "scala-library"))]
    sources = glob.glob(os.path.join(APP, "src", "main", "scala", "**", "*.scala"), recursive=True)
    resources = os.path.join(APP, "src", "main", "resources")
    scalac = os.path.join(scala_home, "bin", "scalac.bat" if os.name == "nt" else "scalac")

    with tempfile.TemporaryDirectory() as classes:
        # The arguments go in an @file: scalac.bat would split a ';'-separated classpath on the command line.
        args_file = os.path.join(classes, "scalac.args")
        with open(args_file, "w", encoding="utf-8") as f:
            for arg in ["-d", classes, "-classpath", os.pathsep.join(jars)] + sources:
                f.write('"' + arg.replace("\\", "/") + '"\n')
        subprocess.run([scalac, "@" + args_file], check=True)
        classpath = os.pathsep.join([classes, resources] + scala_lib + jars)
        runs = [
            ["java", "-cp", classpath, "shipping.ShippingApp", "--zone", "north"],
            ["java", "-Dshipping.max-parcels=50", "-cp", classpath, "shipping.ShippingApp", "--zone", "north"],
        ]
        with open(LOG, "w", encoding="utf-8", newline="\n") as log:
            for args in runs:
                result = subprocess.run(args, capture_output=True, text=True)
                log.write(result.stderr.replace("\r\n", "\n"))

    print(LOG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
