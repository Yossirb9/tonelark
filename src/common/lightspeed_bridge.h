/*
    This file is part of Tonelark, a darktable fork.
    Copyright (C) 2026 Tonelark developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

/*
  Chat bridge: lets Claude Code, Codex or Gemini CLI work with the running
  Tonelark through its MCP server (share/darktable/lightspeed/ai/lsmcp.py).

  The MCP server writes requests as JSON files into <config dir>/mcp/in, the
  bridge answers into <config dir>/mcp/out. <config dir>/mcp/tonelark.json
  tells that Tonelark is running (process id, heartbeat).
*/

#include <glib.h>

G_BEGIN_DECLS

/* start watching the request folder (gui thread, after the views are loaded) */
void dt_lsbridge_start(void);

/* stop it and remove the heartbeat file */
void dt_lsbridge_stop(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
