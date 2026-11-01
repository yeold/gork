---
name: commit
description: >
  Write a commit message for the staged changes and commit them.
  Use when asked to commit.
---
1. Run `git diff --cached --stat` and `git diff --cached` to see what is staged.
   If nothing is, say so and stop.
2. Write a message: a summary line under 60 characters, a blank line, then
   what changed and why, wrapped at 72 columns.
3. Show the message and ask before running `git commit -F -` with it.
