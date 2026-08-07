#!/bin/bash
# Check that large capture files are tracked by git-lfs

# Get staged capture files
staged_files=$(git diff --cached --name-only -- 'test/captures/*.sigmf-data' 'test/captures/*.iq' 2>/dev/null)

for f in $staged_files; do
    # Skip deleted files
    [ -f "$f" ] || continue

    # Check if file is tracked by LFS
    if ! git check-attr filter "$f" | grep -q "filter: lfs"; then
        echo "ERROR: $f is not tracked by git-lfs"
        echo "Add to .gitattributes or run: git lfs track '$f'"
        exit 1
    fi
done

exit 0
