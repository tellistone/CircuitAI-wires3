--------------------------------------------------------------------------------
-- CircuitAI sides: draws the choke_overlay "sides" layer that CircuitAI sends.
--
-- Install: copy this file into your Zero-K data folder's LuaUI/Widgets/ (local widgets are on by default), then in
-- a game against CircuitAI say "choke_overlay sides" in ally chat. It only draws in games with no human opponents,
-- on the machine that runs the AI.
--
-- A region belongs to the side with buildings in it. Both sides' buildings make it contested, and so does the other
-- side's threat there (armed units) at a third of the owner's or more. With no buildings it is nobody's, whatever
-- armies stand in it.
-- A choke is defended by a side whose towers that hit ground cover all of its gap, wall to wall. The other side's
-- towers reaching any of it, or its mobiles there at a third of the defender's threat or more, contest it. Where no
-- side covers all of it, it is nobody's.
-- Regions are tinted by owner: blue ours, red the enemy's, striped blue and red contested, clear nobody's. The tint
-- is stronger where the lines take the region as the owner's ground (below). Thin lines are region borders.
-- Each choke is a gate drawn across its gap, wall to wall:
-- - its core is its owner: blue ours, red the enemy's, dashed blue and red contested, thin grey nobody's;
-- - posts at its ends when towers reach any of it: blue ours, red the enemy's, half and half both; no posts, none;
-- - a casing round the core when it is on a line: green our line where its towers cover what it wants, orange where
--   it's short; the enemy line dark red where their ground-hitting towers cover all of the gap (defended), pink where
--   they don't (undefended); a white rim round the casing on the front line.
-- The lines go round the ground each side owns (as tinted, once that has held 20 s), the enemy's side of the map
-- counting as theirs where nobody owns it, and each side's ground only where it joins that side's main ground (on its
-- side of the map) without crossing the other's: an army camped deep in the other's ground is an island.
-- - our line: every choke out of our ground into a region the enemy can walk to without crossing our ground;
-- - the enemy line: every choke out of their ground into a region we can walk to without crossing theirs;
-- - the front line: the chokes where our ground meets theirs, neither side contested.
-- Each enemy the AI counts is marked where it believes it is: a circle for an armed unit, a square for a building
-- (red armed, pale unarmed), filled while in its sight or radar, an outline where it was last seen. An enemy you see
-- with no mark is one the AI doesn't know of, so a spectator with full view can tell what the overlay is built on.
-- When the AI's "LOS cheating" option is on it knows every enemy unit, and the bottom line says so in red.
-- A legend is printed bottom left.
-- Info boxes give the numbers behind an owner: hostile and allied threat (total, mobile, static) and eco. Hover over a
-- region or near a choke for its boxes beside the cursor; zoomed in, every owned choke and region near the camera
-- gets one beside it, as many as fit. Say "choke_overlay labels" to the AI to turn them off and on.
--
-- With several AI ally teams sending (spectating AI vs AI), it keeps showing one: the local player's, else the
-- lowest ally team id, until that one clears or goes quiet. The ally team shown is printed bottom left.
--
-- Messages (CChokeOverlay::SendWidgetData in src/circuit/terrain/ChokeSides.cpp), "circuit_sides_<verb>:<ally team>"
-- then a body: hello, grid, state, control, lines, info, known, status, clear.
--------------------------------------------------------------------------------

function widget:GetInfo()
	return {
		name    = "CircuitAI sides",
		desc    = "Draws CircuitAI's choke_overlay sides layer: who controls each region and choke, and the lines",
		author  = "CircuitAI",
		date    = "2026-11-18",
		license = "GNU GPL, v2 or later",
		layer   = 0,
		enabled = true,
	}
end

--------------------------------------------------------------------------------
-- Look

local BLUE = {0.25, 0.45, 1.00}
local RED = {1.00, 0.22, 0.18}
local function Tint(colour, alpha)
	return {colour[1], colour[2], colour[3], alpha}
end

local FILL = {  -- by region letter; u (nobody's) isn't drawn
	o = Tint(BLUE, 0.16),  -- ours
	O = Tint(BLUE, 0.30),  -- ours, and the lines take it as ours
	e = Tint(RED, 0.16),  -- the enemy's
	E = Tint(RED, 0.30),  -- the enemy's, and the lines take it as theirs
	cA = Tint(BLUE, 0.26),  -- contested: blue stripes
	cB = Tint(RED, 0.26),  -- and red stripes
}
local STRIPE_CELLS = 3  -- each stripe this many cells wide, on the diagonal
local FILL_LIFT = 4  -- elmos: fills sample the ground only at cell corners, so they float a little over it

local BORDER_COLOUR = {0.85, 0.85, 0.85, 0.35}
local BORDER_WIDTH = 1

local GATE = {  -- a choke's core by who controls it; dashes take the colours in turn along it
	o = {colours = {{0.35, 0.60, 1.00, 1.00}}, width = 5},
	e = {colours = {{1.00, 0.25, 0.20, 1.00}}, width = 5},
	c = {colours = {{0.35, 0.60, 1.00, 1.00}, {1.00, 0.25, 0.20, 1.00}}, width = 5},
	n = {colours = {{0.85, 0.85, 0.85, 0.90}}, width = 2},
}
local GATE_DASH = 40  -- elmos per dash on a contested gate
local GATE_SHADOW = {0.00, 0.00, 0.00, 0.55}  -- under every core, so it reads on any ground
local GATE_SHADOW_WIDTH = 9
local POSTS = {  -- by whose towers reach the choke; n (nobody's) has none
	o = {{0.35, 0.60, 1.00, 1.00}},
	e = {{1.00, 0.25, 0.20, 1.00}},
	b = {{0.35, 0.60, 1.00, 1.00}, {1.00, 0.25, 0.20, 1.00}},
}
local POST_RADIUS = 30  -- elmos
local POST_RIM = {0.05, 0.05, 0.05, 0.90}

local KNOWN = {  -- enemy marks by kind: m an armed mobile, s an armed building, e an unarmed one
	m = {sides = 12, size = 36, colour = {1.00, 0.25, 0.20, 0.90}},
	s = {sides = 4, size = 40, colour = {1.00, 0.25, 0.20, 0.90}},
	e = {sides = 4, size = 32, colour = {1.00, 0.72, 0.62, 0.90}},
}
local KNOWN_RIM = {0.05, 0.05, 0.05, 0.90}
local KNOWN_OUTLINE_WIDTH = 3  -- a remembered one's outline

local CASING_WIDTH = 15
local RIM_WIDTH = 25  -- the front line's casing: a white rim round the others
local LINES = {  -- casings by line kind, drawn in this order: the last one drawn shows on a choke in several lines
	{kind = "f", colour = {1.00, 1.00, 1.00, 1.00}, width = RIM_WIDTH},  -- front line: a rim under the other casings
	{kind = "e", colour = {0.70, 0.08, 0.10, 0.95}},  -- enemy line, defended
	{kind = "u", colour = {1.00, 0.62, 0.78, 0.95}},  -- enemy line, undefended
	{kind = "c", colour = {0.20, 0.80, 0.30, 0.95}},  -- our line, towers cover what it wants
	{kind = "s", colour = {1.00, 0.55, 0.05, 0.95}},  -- our line, short
}

local LIFT = 10  -- lines float this high over the ground
local STEP = 16  -- elmos between line vertices, so lines follow the ground
local STALE_FRAMES = 30 * 30  -- data the AI hasn't refreshed in 30 s isn't drawn

local TEXT_SCALE_ROWS = 1080  -- text sizes are for a screen this many pixels high, and scale with it
local LEGEND_SIZE = 13
local BOX_SIZE = 12
local BOX_RANGE = 2500  -- elmos from the camera: owned chokes and regions this close get boxes
local BOX_MAX = 24  -- at most this many of those
local HOVER_RANGE = 200  -- elmos from a gate: the cursor is at that choke
local BOX_BACK = {0.03, 0.03, 0.05, 0.82}
local BOX_DIM = {0.70, 0.70, 0.70}
local BOX_ZERO = {0.42, 0.42, 0.42}
local OWNERS = {  -- by owner letter
	n = {name = "nobody's", colour = {0.80, 0.80, 0.80}},
	o = {name = "ours", colour = {0.45, 0.65, 1.00}},
	e = {name = "the enemy's", colour = {1.00, 0.35, 0.30}},
	c = {name = "contested", colour = {0.85, 0.65, 1.00}},
}

--------------------------------------------------------------------------------
-- State

local PREFIX = "circuit_sides_"

local spGetGroundHeight = Spring.GetGroundHeight
local spGetGameFrame = Spring.GetGameFrame
local spGetCameraPosition = Spring.GetCameraPosition
local spWorldToScreenCoords = Spring.WorldToScreenCoords
local glVertex = gl.Vertex
local floor, max, min, sqrt = math.floor, math.max, math.min, math.sqrt

local teams = {}  -- per ally team: what its AI sent
local font = nil

local function GroundY(x, z)
	return max(spGetGroundHeight(x, z), 0) + LIFT
end

local function DeleteList(team, key)
	if team[key] then
		gl.DeleteList(team[key])
		team[key] = nil
	end
end

local function DeleteLists(team)
	for _, lists in pairs(team.regionLists or {}) do
		gl.DeleteList(lists[1])
		gl.DeleteList(lists[2])
	end
	team.regionLists = nil
	DeleteList(team, "borderList")
	DeleteList(team, "shadowList")
	DeleteList(team, "gateList")
	DeleteList(team, "lineList")
	DeleteList(team, "knownList")
end

-- Vertices of a ground-following line from (x1, z1) to (x2, z2), as GL.LINES pairs
local function GroundSegment(x1, z1, x2, z2)
	local dx, dz = x2 - x1, z2 - z1
	local steps = max(1, floor(sqrt(dx * dx + dz * dz) / STEP))
	local px, pz = x1, z1
	local py = GroundY(px, pz)
	for i = 1, steps do
		local t = i / steps
		local x, z = x1 + dx * t, z1 + dz * t
		local y = GroundY(x, z)
		glVertex(px, py, pz)
		glVertex(x, y, z)
		px, py, pz = x, y, z
	end
end

--------------------------------------------------------------------------------
-- Building the display lists

-- Per region, two display lists (the cells of stripe A and of stripe B), a quad per cell with the ground's height at
-- its corners: region cells are passable, so gentle enough for that. Built once per grid; a state change only picks
-- the colours they are drawn in
local function BuildRegions(team)
	for _, lists in pairs(team.regionLists or {}) do
		gl.DeleteList(lists[1])
		gl.DeleteList(lists[2])
	end
	local width, height, cell, regionOf = team.width, team.height, team.cell, team.regionOf
	local corner = {}  -- (width + 1) x (height + 1) heights
	for z = 0, height do
		for x = 0, width do
			corner[z * (width + 1) + x] = max(spGetGroundHeight(x * cell, z * cell), 0) + FILL_LIFT
		end
	end
	local cells = {}  -- region -> stripe (1, 2) -> cell indices
	for z = 0, height - 1 do
		for x = 0, width - 1 do
			local region = regionOf[z * width + x + 1]
			if region and region >= 0 then
				local byStripe = cells[region]
				if not byStripe then
					byStripe = {{}, {}}
					cells[region] = byStripe
				end
				local list = byStripe[(floor((x + z) / STRIPE_CELLS) % 2) + 1]
				list[#list + 1] = z * width + x
			end
		end
	end
	local function drawCells(list)
		gl.BeginEnd(GL.QUADS, function()
			for i = 1, #list do
				local index = list[i]
				local x, z = index % width, floor(index / width)
				local x0, z0, x1, z1 = x * cell, z * cell, (x + 1) * cell, (z + 1) * cell
				local c = z * (width + 1) + x
				glVertex(x0, corner[c], z0)
				glVertex(x0, corner[c + width + 1], z1)
				glVertex(x1, corner[c + width + 2], z1)
				glVertex(x1, corner[c + 1], z0)
			end
		end)
	end
	local lists = {}
	for region, byStripe in pairs(cells) do
		lists[region] = {gl.CreateList(drawCells, byStripe[1]), gl.CreateList(drawCells, byStripe[2])}
	end
	team.regionLists = lists
end

-- Thin lines along every cell edge between two regions (walls get none)
local function BuildBorders(team)
	DeleteList(team, "borderList")
	if not team.regionOf then
		return
	end
	local width, height, cell, regionOf = team.width, team.height, team.cell, team.regionOf
	local function regionAt(x, z)
		return regionOf[z * width + x + 1] or -1
	end
	local function isEdge(a, b)
		return a >= 0 and b >= 0 and a ~= b
	end
	local segments = {}
	for z = 1, height - 1 do  -- edges along x, between rows z - 1 and z
		local start = nil
		for x = 0, width do
			local edge = (x < width) and isEdge(regionAt(x, z - 1), regionAt(x, z))
			if edge and not start then
				start = x
			elseif not edge and start then
				segments[#segments + 1] = {start * cell, z * cell, x * cell, z * cell}
				start = nil
			end
		end
	end
	for x = 1, width - 1 do  -- edges along z, between columns x - 1 and x
		local start = nil
		for z = 0, height do
			local edge = (z < height) and isEdge(regionAt(x - 1, z), regionAt(x, z))
			if edge and not start then
				start = z
			elseif not edge and start then
				segments[#segments + 1] = {x * cell, start * cell, x * cell, z * cell}
				start = nil
			end
		end
	end
	team.borderList = gl.CreateList(function()
		gl.BeginEnd(GL.LINES, function()
			for i = 1, #segments do
				local s = segments[i]
				GroundSegment(s[1], s[2], s[3], s[4])
			end
		end)
	end)
end

-- Ground-following GL.LINES from (x1, z1) to (x2, z2), coloured in turn every dash elmos
local function DashedSegment(x1, z1, x2, z2, colours, dash)
	local dx, dz = x2 - x1, z2 - z1
	local length = sqrt(dx * dx + dz * dz)
	local steps = max(1, floor(length / STEP + 0.5))
	for k = 0, steps - 1 do
		local t0, t1 = k / steps, (k + 1) / steps
		gl.Color(colours[floor((t0 + t1) * 0.5 * length / dash) % #colours + 1])
		local ax, az, bx, bz = x1 + dx * t0, z1 + dz * t0, x1 + dx * t1, z1 + dz * t1
		glVertex(ax, GroundY(ax, az), az)
		glVertex(bx, GroundY(bx, bz), bz)
	end
end

-- A disc on the ground at (x, z), in one colour or split in two halves along (ux, uz); a dark rim round it
local POST_SIDES = 16
local function Post(x, z, ux, uz, colours)
	local angle0 = math.atan2(uz, ux)
	local rim = {}
	for i = 0, POST_SIDES do
		local a = angle0 + 2 * math.pi * i / POST_SIDES
		local px, pz = x + math.cos(a) * POST_RADIUS, z + math.sin(a) * POST_RADIUS
		rim[i] = {px, GroundY(px, pz) + 2, pz}
	end
	local cy = GroundY(x, z) + 2
	gl.BeginEnd(GL.TRIANGLES, function()
		for i = 0, POST_SIDES - 1 do
			gl.Color(colours[(i < POST_SIDES / 2) and 1 or #colours])
			glVertex(x, cy, z)
			glVertex(rim[i][1], rim[i][2], rim[i][3])
			glVertex(rim[i + 1][1], rim[i + 1][2], rim[i + 1][3])
		end
	end)
	gl.Color(POST_RIM)
	gl.BeginEnd(GL.LINE_LOOP, function()
		for i = 0, POST_SIDES - 1 do
			glVertex(rim[i][1], rim[i][2], rim[i][3])
		end
	end)
end

-- Each choke's gate: a shadow under it (its own list, drawn before the line casings), then the core and the posts
local function BuildGates(team)
	DeleteList(team, "shadowList")
	DeleteList(team, "gateList")
	local gates = team.gates or {}
	team.shadowList = gl.CreateList(function()
		gl.Color(GATE_SHADOW)
		gl.LineWidth(GATE_SHADOW_WIDTH)
		gl.BeginEnd(GL.LINES, function()
			for i = 1, #gates do
				local g = gates[i]
				GroundSegment(g[1], g[2], g[3], g[4])
			end
		end)
		gl.LineWidth(1)
	end)
	team.gateList = gl.CreateList(function()
		for who, style in pairs(GATE) do
			gl.LineWidth(style.width)
			gl.BeginEnd(GL.LINES, function()
				for i = 1, #gates do
					local g = gates[i]
					if g.who == who then
						DashedSegment(g[1], g[2], g[3], g[4], style.colours, GATE_DASH)
					end
				end
			end)
		end
		gl.LineWidth(2)
		for i = 1, #gates do
			local g = gates[i]
			local colours = POSTS[g.defended]
			if colours then
				local dx, dz = g[3] - g[1], g[4] - g[2]
				local length = sqrt(dx * dx + dz * dz)
				if length < 3 * POST_RADIUS then
					Post((g[1] + g[3]) * 0.5, (g[2] + g[4]) * 0.5, 1, 0, colours)
				else
					local ux, uz = dx / length, dz / length
					Post(g[1], g[2], ux, uz, colours)
					Post(g[3], g[4], ux, uz, colours)
				end
			end
		end
		gl.LineWidth(1)
	end)
end

-- Each known enemy's mark: a filled shape with a dark rim while seen, an outline where it was last seen
local function BuildKnown(team)
	DeleteList(team, "knownList")
	local known = team.known or {}
	team.knownList = gl.CreateList(function()
		for i = 1, #known do
			local k = known[i]
			local style = KNOWN[k.kind]
			local x, z = k[1], k[2]
			local y = GroundY(x, z) + 3
			local rim = {}
			for s = 0, style.sides - 1 do
				local a = 2 * math.pi * (s + 0.5) / style.sides
				local r = (style.sides == 4) and style.size * sqrt(2) or style.size
				rim[s] = {x + math.cos(a) * r, z + math.sin(a) * r}
			end
			if k.isNow then
				gl.Color(style.colour)
				gl.BeginEnd(GL.TRIANGLES, function()
					for s = 0, style.sides - 1 do
						local a, b = rim[s], rim[(s + 1) % style.sides]
						glVertex(x, y, z)
						glVertex(a[1], y, a[2])
						glVertex(b[1], y, b[2])
					end
				end)
				gl.Color(KNOWN_RIM)
				gl.LineWidth(2)
			else  -- dark under the outline, so it reads on any ground
				gl.Color(KNOWN_RIM)
				gl.LineWidth(KNOWN_OUTLINE_WIDTH + 2)
				gl.BeginEnd(GL.LINE_LOOP, function()
					for s = 0, style.sides - 1 do
						glVertex(rim[s][1], y, rim[s][2])
					end
				end)
				gl.Color(style.colour)
				gl.LineWidth(KNOWN_OUTLINE_WIDTH)
			end
			gl.BeginEnd(GL.LINE_LOOP, function()
				for s = 0, style.sides - 1 do
					glVertex(rim[s][1], y, rim[s][2])
				end
			end)
		end
		gl.LineWidth(1)
	end)
end

local function BuildLines(team)
	DeleteList(team, "lineList")
	local lines = team.lines or {}
	team.lineList = gl.CreateList(function()
		for _, style in ipairs(LINES) do
			gl.LineWidth(style.width or CASING_WIDTH)
			gl.Color(style.colour)
			gl.BeginEnd(GL.LINES, function()
				for i = 1, #lines do
					local l = lines[i]
					if l.kind == style.kind then
						GroundSegment(l[1], l[2], l[3], l[4])
					end
				end
			end)
		end
		gl.LineWidth(1)
	end)
end

--------------------------------------------------------------------------------
-- Messages

local function Numbers(text)
	local result = {}
	for n in text:gmatch("%-?%d+") do
		result[#result + 1] = tonumber(n)
	end
	return result
end

local function Records(body)
	local result = {}
	for record in body:gmatch("[^\n]+") do
		result[#result + 1] = record
	end
	return result
end

local handlers = {}

function handlers.hello(team)
	return team.regionOf and "1" or "0"  -- "0": send the grid
end

function handlers.grid(team, body)
	local n = Numbers(body)
	if #n < 3 then
		return
	end
	team.width, team.height, team.cell = n[1], n[2], n[3]
	local regionOf, at = {}, 0
	for i = 4, #n - 1, 2 do
		local region, count = n[i], n[i + 1]
		for k = 1, count do
			regionOf[at + k] = region
		end
		at = at + count
	end
	team.regionOf = regionOf
	BuildBorders(team)
	BuildRegions(team)
end

function handlers.state(team, body)
	team.state = body:sub(2)
end

function handlers.control(team, body)
	local gates = {}
	for _, record in ipairs(Records(body)) do
		local who, defended, x1, z1, x2, z2 = record:match("^(%a) (%a) (%-?%d+) (%-?%d+) (%-?%d+) (%-?%d+)")
		if who and GATE[who] then
			gates[#gates + 1] = {tonumber(x1), tonumber(z1), tonumber(x2), tonumber(z2), who = who, defended = defended}
		end
	end
	team.gates = gates
	BuildGates(team)
end

function handlers.lines(team, body)
	local lines = {}
	for _, record in ipairs(Records(body)) do
		local kind, x1, z1, x2, z2 = record:match("^(%a) (%-?%d+) (%-?%d+) (%-?%d+) (%-?%d+)")
		if kind then
			lines[#lines + 1] = {tonumber(x1), tonumber(z1), tonumber(x2), tonumber(z2), kind = kind}
		end
	end
	team.lines = lines
	BuildLines(team)
end

function handlers.status(team, body)
	team.isCheating = (body:find("cheating") ~= nil)
end

function handlers.known(team, body)
	local known = {}
	for _, record in ipairs(Records(body)) do
		local kind, x, z = record:match("^(%a) (%-?%d+) (%-?%d+)")
		if kind and KNOWN[kind:lower()] then
			known[#known + 1] = {tonumber(x), tonumber(z), kind = kind:lower(), isNow = (kind ~= kind:lower())}
		end
	end
	team.known = known
	BuildKnown(team)
end

-- Per region and choke: key x z owner, then hostile mobile, static, eco, allied the same, then its title lines
function handlers.info(team, body)
	local regions, chokes = {}, {}
	for _, record in ipairs(Records(body)) do
		local kind, index, x, z, owner, rest = record:match("^(%a)(%d+) (%-?%d+) (%-?%d+) (%a) (.*)$")
		if kind and OWNERS[owner] then
			local values = {}
			for i = 1, 6 do
				local value, tail = rest:match("^(%-?[%d%.]+) ?(.*)$")
				if not value then
					break
				end
				values[i], rest = tonumber(value), tail
			end
			if #values == 6 then
				local lines = {}
				for line in rest:gmatch("[^|]+") do
					lines[#lines + 1] = line
				end
				local item = {
					x = tonumber(x), z = tonumber(z), owner = owner, lines = lines, isChoke = (kind == "c"),
					hostile = {mobile = values[1], static = values[2], eco = values[3]},
					allied = {mobile = values[4], static = values[5], eco = values[6]},
				}
				if kind == "c" then
					chokes[tonumber(index)] = item
				else
					regions[tonumber(index)] = item
				end
			end
		end
	end
	team.regionInfo, team.chokeInfo = regions, chokes
end

function widget:RecvSkirmishAIMessage(aiTeam, dataStr)
	if dataStr:sub(1, #PREFIX) ~= PREFIX then
		return
	end
	local verb, ally, body = dataStr:match("^circuit_sides_(%a+):(%-?%d+)(.*)$")
	ally = tonumber(ally)
	if not verb or not ally then
		return
	end
	if verb == "clear" then
		if teams[ally] then
			DeleteLists(teams[ally])
			teams[ally] = nil
		end
		return "1"
	end
	local handler = handlers[verb]
	if not handler then
		return
	end
	local team = teams[ally]
	if not team then
		team = {}
		teams[ally] = team
	end
	team.frame = spGetGameFrame()
	return handler(team, body) or "1"
end

--------------------------------------------------------------------------------
-- Drawing

local shown = nil  -- the ally team drawn: kept until it clears or goes quiet

local function IsFresh(ally)
	local team = teams[ally]
	return team and (spGetGameFrame() - (team.frame or 0) <= STALE_FRAMES)
end

local function ShownTeam()
	if not (shown and IsFresh(shown)) then
		shown = nil
		local isSpectator = Spring.GetSpectatingState()
		local mine = Spring.GetMyAllyTeamID()
		if not isSpectator and IsFresh(mine) then
			shown = mine
		else
			for ally in pairs(teams) do
				if IsFresh(ally) and (not shown or ally < shown) then
					shown = ally
				end
			end
		end
	end
	return shown and teams[shown]
end

function widget:DrawWorldPreUnit()
	local team = ShownTeam()
	if not team then
		return
	end
	gl.Blending(GL.SRC_ALPHA, GL.ONE_MINUS_SRC_ALPHA)
	gl.DepthTest(true)
	gl.DepthMask(false)
	if team.regionLists and team.state then
		gl.PolygonOffset(-25, -2)
		local state = team.state
		for region, lists in pairs(team.regionLists) do
			local letter = state:sub(region + 1, region + 1)
			if letter == "c" or letter == "C" then
				gl.Color(FILL.cA)
				gl.CallList(lists[1])
				gl.Color(FILL.cB)
				gl.CallList(lists[2])
			elseif FILL[letter] then
				gl.Color(FILL[letter])
				gl.CallList(lists[1])
				gl.CallList(lists[2])
			end
		end
		gl.PolygonOffset(false)
	end
	if team.borderList then
		gl.Color(BORDER_COLOUR)
		gl.LineWidth(BORDER_WIDTH)
		gl.CallList(team.borderList)
		gl.LineWidth(1)
	end
	if team.shadowList then
		gl.CallList(team.shadowList)
	end
	if team.lineList then
		gl.CallList(team.lineList)
	end
	if team.gateList then
		gl.CallList(team.gateList)
	end
	if team.knownList then
		gl.CallList(team.knownList)
	end
	gl.DepthMask(true)
	gl.DepthTest(false)
	gl.Color(1, 1, 1, 1)
end

-- Colour codes for font:Print
local function Ink(colour)
	local function byte(v)
		return string.char(max(1, min(255, floor(v * 255))))
	end
	return "\255" .. byte(colour[1]) .. byte(colour[2]) .. byte(colour[3])
end

local function MakeLegend()
	local w, blue, red = Ink({1, 1, 1}), Ink(GATE.o.colours[1]), Ink(GATE.e.colours[1])
	local casing = {}
	for _, style in ipairs(LINES) do
		casing[style.kind] = Ink(style.colour)
	end
	local sides = blue .. "ours" .. w .. ", " .. red .. "enemy" .. w
	return {
		"Casing: " .. casing.c .. "our line" .. w .. " / " .. casing.s .. "short" .. w .. ", " .. casing.f .. "rimmed"
				.. w .. " front line, " .. casing.e .. "enemy line defended" .. w .. " / " .. casing.u .. "undefended",
		"Posts at a gate's ends: towers reach part of it (" .. sides .. ", half and half both)",
		"Choke gates: defended where one side's towers cover all of the gap (" .. sides .. "), dashed contested"
				.. " (both, or the other's towers or 1/3 of the threat in mobiles), grey nobody's",
		"Regions: the side with buildings there (" .. sides .. "), striped contested (both, or 1/3 of the threat in"
				.. " the other's units), clear no buildings",
		"Strong tint: the lines take it (held 20 s, joined to its side's main ground). Hover for the numbers",
		"Enemies the AI counts: " .. red .. "circle" .. w .. " unit, " .. red .. "square" .. w .. " building ("
				.. Ink(KNOWN.e.colour) .. "pale" .. w .. " unarmed), filled seen now, outline last seen. Unmarked: unknown to it",
	}
end
local legend = nil

local function TextScale()
	local _, vsy = gl.GetViewSizes()
	return max(0.75, vsy / TEXT_SCALE_ROWS)
end

-- Info boxes

local function Amount(value)
	if value >= 9999.5 then
		return string.format("%.0fk", value / 1000)
	elseif value >= 999.5 then
		return string.format("%.1fk", value / 1000)
	elseif value >= 99.95 or value == floor(value) then
		return string.format("%.0f", value)
	end
	return string.format("%.1f", value)
end

-- A box's text and size, at font size 1: rows of {text, x, align}, x a fraction of the box's inner width or offset
local function MakeBox(item)
	local owner = OWNERS[item.owner]
	local h, a = item.hostile, item.allied
	local dim, zero = Ink(BOX_DIM), Ink(BOX_ZERO)
	local function number(value, ink)
		return (value > 0) and ((ink or "") .. Amount(value)) or (zero .. "0")
	end
	local rows = {
		{label = Ink(owner.colour) .. (item.lines[1] or "") .. ": " .. owner.name},
		{label = "", hostile = Ink(RED) .. "hostile", allied = Ink(BLUE) .. "allied"},
		{label = "threat", hostile = number(h.mobile + h.static), allied = number(a.mobile + a.static)},
		{label = dim .. "  mobile", hostile = number(h.mobile, dim), allied = number(a.mobile, dim)},
		{label = dim .. "  static", hostile = number(h.static, dim), allied = number(a.static, dim)},
		{label = "eco", hostile = number(h.eco), allied = number(a.eco)},
	}
	for i = 2, #item.lines do
		rows[#rows + 1] = {label = dim .. item.lines[i]}
	end
	local labelWidth, columnWidth, lineWidth = 0, 0, 0
	for i, row in ipairs(rows) do
		local width = font:GetTextWidth(row.label)
		if row.hostile then
			labelWidth = max(labelWidth, width)
			columnWidth = max(columnWidth, font:GetTextWidth(row.hostile), font:GetTextWidth(row.allied))
		else
			lineWidth = max(lineWidth, width)
		end
	end
	local gap = 0.8
	local tableWidth = labelWidth + 2 * (gap + columnWidth)
	return {
		rows = rows, colour = owner.colour,
		width = max(tableWidth, lineWidth), hostileX = labelWidth + gap + columnWidth, alliedX = tableWidth,
	}
end

-- The cursor's region and choke (the one whose gate is nearest, within HOVER_RANGE)
local function Hovered(team)
	local mx, my, _, _, _, offscreen = Spring.GetMouseState()
	if offscreen then
		return
	end
	local kind, pos = Spring.TraceScreenRay(mx, my, true)
	if kind ~= "ground" or not pos then
		return
	end
	local x, z = pos[1], pos[3]
	local region, choke
	if team.regionOf then
		local cx, cz = floor(x / team.cell), floor(z / team.cell)
		if cx >= 0 and cz >= 0 and cx < team.width and cz < team.height then
			region = team.regionOf[cz * team.width + cx + 1]
		end
	end
	local best = HOVER_RANGE * HOVER_RANGE
	for index, item in pairs(team.chokeInfo or {}) do
		local g = team.gates and team.gates[index + 1]
		local ax, az, bx, bz = item.x, item.z, item.x, item.z
		if g then
			ax, az, bx, bz = g[1], g[2], g[3], g[4]
		end
		local dx, dz = bx - ax, bz - az
		local length2 = dx * dx + dz * dz
		local t = (length2 > 0) and max(0, min(1, ((x - ax) * dx + (z - az) * dz) / length2)) or 0
		local ex, ez = ax + dx * t - x, az + dz * t - z
		local d2 = ex * ex + ez * ez
		if d2 < best then
			best, choke = d2, index
		end
	end
	return region, choke, mx, my
end

local function Overlaps(a, b)
	return a[1] < b[3] and b[1] < a[3] and a[2] < b[4] and b[2] < a[4]
end

-- Boxes: the cursor's choke and region beside it, then owned chokes and regions near the camera, nearest first, each
-- beside its point and where it overlaps no box already placed
local function DrawBoxes(team)
	if not (team.regionInfo or team.chokeInfo) then
		return
	end
	local vsx, vsy = gl.GetViewSizes()
	local size = floor(BOX_SIZE * TextScale() + 0.5)
	local pad, lineHeight = floor(size * 0.5), floor(size * 1.3)
	local placed = {}
	local function boxOf(item)
		item.box = item.box or MakeBox(item)
		return item.box, item.box.width * size + 2 * pad, #item.box.rows * lineHeight + 2 * pad
	end
	local function place(item, left, top, anchorX, anchorY, isForced)
		local box, width, height = boxOf(item)
		left, top = max(0, min(vsx - width, left)), max(height, min(vsy, top))
		local rect = {left, top - height, left + width, top}
		if not isForced then
			for i = 1, #placed do
				if Overlaps(rect, placed[i].rect) then
					return false
				end
			end
		end
		placed[#placed + 1] = {rect = rect, box = box, anchorX = anchorX, anchorY = anchorY}
		return true
	end

	local region, choke, mx, my = Hovered(team)
	local hoveredChoke = choke and team.chokeInfo[choke]
	local hoveredRegion = region and region >= 0 and team.regionInfo and team.regionInfo[region]
	local gapX, gapY = floor(size * 1.5), floor(size * 0.5)
	if hoveredChoke then  -- above the cursor
		local _, _, height = boxOf(hoveredChoke)
		place(hoveredChoke, mx + gapX, my + gapY + height, nil, nil, true)
	end
	if hoveredRegion then  -- below it
		place(hoveredRegion, mx + gapX, my - gapY, nil, nil, true)
	end

	local cx, cy, cz = spGetCameraPosition()
	local near = {}
	local function consider(item)
		if item.owner ~= "n" and item ~= hoveredChoke and item ~= hoveredRegion then
			local y = GroundY(item.x, item.z)
			local dx, dy, dz = item.x - cx, y - cy, item.z - cz
			local d2 = dx * dx + dy * dy + dz * dz
			if d2 <= BOX_RANGE * BOX_RANGE then
				near[#near + 1] = {item = item, y = y, rank = d2 + (item.isChoke and 0 or BOX_RANGE * BOX_RANGE)}
			end
		end
	end
	for _, item in pairs(team.chokeInfo or {}) do
		consider(item)
	end
	for _, item in pairs(team.regionInfo or {}) do
		consider(item)
	end
	table.sort(near, function(a, b) return a.rank < b.rank end)
	local count = 0
	for i = 1, #near do
		if count >= BOX_MAX then
			break
		end
		local item = near[i].item
		local sx, sy, sz = spWorldToScreenCoords(item.x, near[i].y, item.z)
		if sz and sz < 1 and sx >= 0 and sx <= vsx and sy >= 0 and sy <= vsy then
			if place(item, sx + gapY, sy - gapY, sx, sy) then
				count = count + 1
			end
		end
	end
	if #placed == 0 then
		return
	end

	for i = 1, #placed do
		local p = placed[i]
		local r = p.rect
		gl.Color(BOX_BACK)
		gl.Rect(r[1], r[2], r[3], r[4])
		gl.Color(p.box.colour[1], p.box.colour[2], p.box.colour[3], 0.9)
		gl.Rect(r[1], r[4] - 2, r[3], r[4])  -- a bar along the top in the owner's colour
		gl.Rect(r[1], r[2], r[1] + 1, r[4])
		gl.Rect(r[3] - 1, r[2], r[3], r[4])
		gl.Rect(r[1], r[2], r[3], r[2] + 1)
		if p.anchorX then
			gl.Rect(p.anchorX - 3, p.anchorY - 3, p.anchorX + 3, p.anchorY + 3)
		end
	end
	font:Begin()
	for i = 1, #placed do
		local p = placed[i]
		local left, top = p.rect[1] + pad, p.rect[4] - pad
		for k, row in ipairs(p.box.rows) do
			local y = top - k * lineHeight + floor((lineHeight - size) * 0.5)
			font:Print(row.label, left, y, size, "o")
			if row.hostile then
				font:Print(row.hostile, left + p.box.hostileX * size, y, size, "ro")
				font:Print(row.allied, left + p.box.alliedX * size, y, size, "ro")
			end
		end
	end
	font:End()
	gl.Color(1, 1, 1, 1)
end

function widget:DrawScreen()
	local team = ShownTeam()
	if not team or not font then
		return
	end
	DrawBoxes(team)
	legend = legend or MakeLegend()
	local size = floor(LEGEND_SIZE * TextScale() + 0.5)
	local lineHeight = floor(size * 1.4)
	font:Begin()
	for i = 1, #legend do
		font:Print(legend[i], 16, 16 + lineHeight * i, size, "o")
	end
	font:Print("CircuitAI sides: ally team " .. shown .. (team.isCheating
			and (Ink(RED) .. "   LOS cheating is on: the AI knows every enemy unit") or ""), 16, 16, size + 1, "o")
	font:End()
end

function widget:Initialize()
	font = gl.LoadFont("FreeSansBold.otf", 64, 8, 6)
end

function widget:Shutdown()
	for _, team in pairs(teams) do
		DeleteLists(team)
	end
	teams = {}
	if font then
		gl.DeleteFont(font)
		font = nil
	end
end
