/*
 * mcp.h -- Model Context Protocol client (stdio servers only).
 */

#ifndef GORK_MCP_H
#define GORK_MCP_H

#include "cJSON/cJSON.h"

/* Start every server in `path` (mcpServers JSON) and list its tools.
   Problems are reported on stderr and that server is skipped; a missing
   file just means no servers. */
void  mcp_start(const char *path);
void  mcp_stop(void);

int   mcp_tool_count(void);

/* Append every MCP tool definition to a request's "tools" array. */
int   mcp_add_tools(cJSON *tools);

/* Index of the tool the model calls `name`, or -1. */
int   mcp_find(const char *name);

/* The tool's "always approve" flag, seeded from autoApprove. */
int  *mcp_always(int i);

/* Call tool i.  Same contract as a builtin tool: malloc'd text, NULL when
   out of memory, *is_err set when the result reports a failure. */
char *mcp_call(int i, cJSON *input, int *is_err);

#endif /* GORK_MCP_H */
