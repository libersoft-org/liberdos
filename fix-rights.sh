#!/bin/sh

find . -type f -executable \
	-not -path "*/build/*" \
	-not -path "*/images/*" \
	-not -path "*/hdd/*" \
	-not -path "*/.git/*" \
	-not -name "*.sh" \
	-exec echo "chmod -x {}" \;
