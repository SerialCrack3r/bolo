#!/bin/sh

MAJOR=0
MINOR=3
PATCH=0

LIB_CURRENT=1
LIB_REVISION=0
LIB_AGE=0

TSDP_CURRENT=1
TSDP_REVISION=0
TSDP_AGE=0

case "$1" in
	package)
		echo "$MAJOR.$MINOR.$PATCH" | tr -d '\n'
		;;
	lib)
		echo "[$LIB_CURRENT], [$LIB_REVISION], [$LIB_AGE]" | tr -d '\n'
		;;
	tsdp)
		echo "[$TSDP_CURRENT], [$TSDP_REVISION], [$TSDP_AGE]" | tr -d '\n'
		;;
	*)
		echo "invalid version call $1"
		echo "Usage: $0 {package|lib|tsdp}"
		exit 1
esac
