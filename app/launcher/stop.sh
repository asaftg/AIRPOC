#!/bin/bash
# Stop the AIRPOC stack — console, EO preview, radar daemon. Deliberately leaves the
# recorder (airpoc-recorder, its own service) and this launcher running so recordings
# survive and control stays reachable to press START again.
pkill -f "[.]/app -p 8080"   2>/dev/null
pkill -f "eo_pipeline"       2>/dev/null
pkill -f "radar_preview"     2>/dev/null
echo "stop requested"
