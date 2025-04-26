#!/bin/bash

TARGET="input_dispi"

if pgrep "$TARGET" > /dev/null; then
    echo "Sending SIGINT to all processes named '$TARGET'..."
    pkill -SIGINT "$TARGET"
    echo "Done."
else
    echo "No process named '$TARGET' found."
fi
