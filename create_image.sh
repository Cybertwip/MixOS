#!/bin/bash

# Archive only the image produced by the active build.  Falling back to all
# images preserves compatibility when this helper is run by itself.
if [[ -n "${DISK:-}" ]]; then
  images=("$DISK")
else
  shopt -s nullglob
  images=(*.img)
fi

for IMAGE in "${images[@]}"
do
  if [ ! -f "${IMAGE}.7z" ] && [ ! -f "${IMAGE}.7z.001" ]; then
    7z a -mmt="${BUILD_JOBS:-4}" -v1950m "${IMAGE}.7z" "$IMAGE"
    #xz --keep -z -9 -T0 -M 80% "${IMAGE}"
  fi
done
