#!/usr/bin/env bash
# Double-click launcher (Git Bash / macOS Terminal / Linux). Builds the app
# with plain `javac` if no jar exists yet, so it works without Maven too.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

JAR=$(ls target/Fanorona-*.jar 2>/dev/null | head -1 || true)

if [ -z "$JAR" ]; then
    echo "No build found — compiling with javac..."
    mkdir -p target/classes
    javac -d target/classes src/main/java/org/willy/*.java
    cp src/main/resources/game.html target/classes/
    printf 'Main-Class: org.willy.FanoronaServer\n' > target/classes/manifest.txt
    jar cfm target/Fanorona.jar target/classes/manifest.txt -C target/classes .
    JAR=target/Fanorona.jar
fi

echo "Starting Fanorona ($JAR)..."
java -jar "$JAR" "$@"

read -r -p "Server stopped. Press Enter to close..." _ || true
