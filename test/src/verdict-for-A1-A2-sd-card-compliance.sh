#!/bin/bash

# Check if parameter is given
if [ -z "$1" ]; then
    echo "Usage: $0 <mount_point>"
    exit 1
fi

CARD="$1"

# Check if mount point exists
if [ ! -d "$CARD" ]; then
    echo "Error: Mount point '$CARD' not found!"
    exit 1
fi

# Run fio test (A1/A2 style random access)
fio --name=A1A2_test \
    --filename="$CARD/testfile" \
    --size=1G \
    --bs=4k \
    --rw=randrw \
    --rwmixread=75 \
    --direct=1 \
    --iodepth=16 \
    --numjobs=4 \
    --runtime=60 \
    --time_based \
    --group_reporting \
    --fallocate=none > result.txt

# Extract read and write IOPS from fio output
READ_IOPS=$(grep -A1 "read:" result.txt | grep IOPS | awk -F'=' '{print $2}' | awk '{print $1}' | tr -d ',')
WRITE_IOPS=$(grep -A1 "write:" result.txt | grep IOPS | awk -F'=' '{print $2}' | awk '{print $1}' | tr -d ',')

# Print results
echo "================ A1/A2 Test Results for $CARD ================"
echo "Random Read IOPS : $READ_IOPS"
echo "Random Write IOPS: $WRITE_IOPS"

# Classification logic
if (( $(echo "$READ_IOPS >= 4000" | bc -l) )) && (( $(echo "$WRITE_IOPS >= 2000" | bc -l) )); then
    echo "🏆 Result: A2 compliant"
elif (( $(echo "$READ_IOPS >= 1500" | bc -l) )) && (( $(echo "$WRITE_IOPS >= 500" | bc -l) )); then
    echo "✅ Result: A1 compliant"
else
    echo "❌ Result: Does not meet A1/A2 requirements"
fi

# Cleanup
rm -f "$CARD/testfile"

