#!/bin/sh
# A stdio MCP server in plain sh, with one tool, echo, for systems without
# Python: testing/vmcheck.sh points gork at it.  It answers by request
# type and does not parse JSON; the reply names the system it runs on.
sys=`uname -s 2>/dev/null`
while read line
do
  id=`echo "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p'`
  case "$line" in
  *'"initialize"'*)
    echo '{"jsonrpc":"2.0","id":'$id',"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{}},"serverInfo":{"name":"sh","version":"1"}}}' ;;
  *'"tools/list"'*)
    echo '{"jsonrpc":"2.0","id":'$id',"result":{"tools":[{"name":"echo","description":"Echo.","inputSchema":{"type":"object","properties":{"text":{"type":"string"}}}}]}}' ;;
  *'"tools/call"'*)
    echo '{"jsonrpc":"2.0","id":'$id',"result":{"content":[{"type":"text","text":"sh on '"$sys"' says hi"}]}}' ;;
  esac
done
