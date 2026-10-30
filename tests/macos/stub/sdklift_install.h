/* Stand-in for build/gen/sdklift_install.h (tools/lifter/sdklift.py), so nnservices.c compiles without a dump: no SDK
 * function is replaced by lifted code (the generated list may be empty too). */
static const char *const sdk_install_names[] = {
  0 };
