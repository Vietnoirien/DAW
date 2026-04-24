#!/bin/bash
gdb -q -ex "set confirm off" -ex "run" -ex "bt full" -ex "quit" ./build/LiBeDAW_artefacts/LiBeDAW
