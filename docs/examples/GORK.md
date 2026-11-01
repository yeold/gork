# Project rules

gork sends this file, from the directory it runs in, as the system prompt
of every request. Write what the model should know about the project.

- This is ANSI C89: no // comments, no declarations after statements, no
  long long.
- Build with `make`; run the tests with `make check` before saying a change
  works.
- Keep lines under 80 columns and follow the style of the file you edit.
- Do not touch files under vendor/.
