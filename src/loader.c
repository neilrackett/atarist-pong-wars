/**
 * Pong Wars loader for AUTO folder
 */

#include <string.h>
#include <osbind.h>

int main(int argc, char *argv[])
{
  /* TOS paths are short; 128 chars is plenty for typical use. */
  char fullpath[128];
  const char *self;
  const char *target = "PONGWARS.TOS";
  char *sep;
  long rc;

  /* Best guess at our own path */
  if (argc > 0 && argv[0] && argv[0][0])
  {
    self = argv[0];
  }
  else
  {
    /* Fallback: hope current directory already is the right one */
    self = target;
  }

  /* Copy argv[0] so we can chop the filename off */
  strncpy(fullpath, self, sizeof(fullpath) - 1);
  fullpath[sizeof(fullpath) - 1] = '\0';

  /* Find last backslash in the path (Atari ST uses '\') */
  sep = strrchr(fullpath, '\\');

  if (sep)
  {
    /* Keep drive + directory, then append our target name */
    ++sep;       /* move past '\' */
    *sep = '\0'; /* terminate after the slash */

    strncat(fullpath, target,
            sizeof(fullpath) - strlen(fullpath) - 1);
  }
  else
  {
    /* No directory info in argv[0], just use plain name */
    strncpy(fullpath, target, sizeof(fullpath) - 1);
    fullpath[sizeof(fullpath) - 1] = '\0';
  }

  /* Load and execute PONGWARS.TOS with no arguments, inherit env */
  rc = Pexec(0, fullpath, "", (void *)0);

  /* We don't care about rc here – just terminate */
  (void)rc;
  return 0;
}
