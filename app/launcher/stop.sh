#!/bin/bash
# Stop the AIRPOC stack — console, EO preview, radar daemon, EO detector. Deliberately
# leaves the recorder (airpoc-recorder, its own service) and this launcher running so
# recordings survive and control stays reachable to press START again.
# Console is matched by cmdline (its comm is the too-generic "app"); the producers by EXACT
# name (-x) so we never kill a log tail, an ssh session, or an in-flight start.sh.
pkill -f "[.]/app -p 8080"   2>/dev/null
pkill -x eo_pipeline         2>/dev/null
pkill -x radar_preview       2>/dev/null
pkill -x detectiond          2>/dev/null
pkill -x trackerd            2>/dev/null
pkill -x fusiond             2>/dev/null
echo "stop requested"
