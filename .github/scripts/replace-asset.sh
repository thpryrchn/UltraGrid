#!/bin/sh -eux

# If first parameter is GITHUB_TOKEN=<tok>, use <tok> as an env var GITHUB_TOKEN (used by the scripts below)
REPOSITORY=$(expr $1 : "GITHUB_REPOSITORY=\(.*\)")
if [ $? -eq 0 ]; then
        export GITHUB_REPOSITORY=$REPOSITORY
        shift
fi
TOKEN=$(expr $1 : "GITHUB_TOKEN=\(.*\)")
if [ $? -eq 0 ]; then
        export GITHUB_TOKEN=$TOKEN
        shift
fi

DIR=$(dirname $0)

$DIR/delete-asset.sh "$@"
$DIR/upload-asset.sh "$@"

