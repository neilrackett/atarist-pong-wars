/**
 * Pong Wars loader for AUTO folder
 *
 * Searches for the first .TOS file in the same directory as the loader
 * and executes it, so the same LOADER.PRG works with any game version.
 */

#include <string.h>
#include <osbind.h>

int main(int argc, char *argv[])
{
  char dir[128];
  char fullpath[128];
  const char *self;
  char *sep;
  _DTA dta;
  long rc;

  /* Best guess at our own directory */
  if (argc > 0 && argv[0] && argv[0][0])
    self = argv[0];
  else
    self = "\\";

  strncpy(dir, self, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';

  sep = strrchr(dir, '\\');
  if (sep)
    *(sep + 1) = '\0'; /* keep trailing backslash */
  else
    dir[0] = '\0';     /* no path prefix — use bare name */

  /* Build search pattern: <dir>*.TOS */
  strncpy(fullpath, dir, sizeof(fullpath) - 1);
  fullpath[sizeof(fullpath) - 1] = '\0';
  strncat(fullpath, "*.TOS", sizeof(fullpath) - strlen(fullpath) - 1);

  Fsetdta(&dta);
  rc = Fsfirst(fullpath, 0x20); /* normal files only */
  if (rc != 0)
    return 1; /* no .TOS found */

  /* Build full path to the found file */
  strncpy(fullpath, dir, sizeof(fullpath) - 1);
  fullpath[sizeof(fullpath) - 1] = '\0';
  strncat(fullpath, dta.dta_name, sizeof(fullpath) - strlen(fullpath) - 1);

  rc = Pexec(0, fullpath, "", (void *)0);
  (void)rc;
  return 0;
}
