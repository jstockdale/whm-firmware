# wh-link v1 - vendored verbatim from the authoritative bundle.
# Header-only core; the IMPLEMENTATION TU lives in main/whlink_glue.c.
# Conformance: run ./verify.sh in this directory on the build host -
# it is part of the release gate for any wh-link-touching build.
idf_component_register(INCLUDE_DIRS ".")
