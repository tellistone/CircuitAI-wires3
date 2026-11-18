--------------------------------------------------------------------------------
-- CircuitAI sides: draws the choke_overlay "sides" layer that CircuitAI sends.
--
-- Install: copy this file into your Zero-K data folder's LuaUI/Widgets/ (local widgets are on by default), then in
-- a game against CircuitAI say "choke_overlay sides" in ally chat. It only draws in games with no human opponents,
-- on the machine that runs the AI.
--
-- Regions behind our current line are tinted blue, beyond the enemy line red, stronger where that side has
-- buildings. Contested regions (between the lines, both sides' armies there now, or both sides' buildings) are
-- striped blue and red. Thin lines are region borders. Our current line is blue where its towers cover what it
-- wants, orange where it's short. The enemy line is red where defended, pink where undefended, grey where not seen
-- enough; enemy-held chokes off it are dark red. The front line has a white halo, the forward line is thin yellow.
--
-- With several AI ally teams sending (spectating AI vs AI), it keeps showing one: the local player's, else the
-- lowest ally team id, until that one clears or goes quiet. The ally team shown is printed bottom left.
--
-- Messages (CChokeOverlay::SendWidgetData in src/circuit/terrain/ChokeSides.cpp), "circuit_sides_<verb>:<ally team>"
-- then a body: hello, grid, state, lines, labels, clear.
--------------------------------------------------------------------------------

function widget:GetInfo()
	return {
		name    = "CircuitAI sides",
		desc    = "Draws CircuitAI's choke_overlay sides layer: who holds each region, the lines, enemy pressure",
		author  = "CircuitAI",
		date    = "2026-11-18",
		license = "GNU GPL, v2 or later",
		layer   = 0,
		enabled = true,
	}
end

--------------------------------------------------------------------------------
-- Look

local FILL = {
	o = {0.25, 0.45, 1.00, 0.16},  -- ours, behind the current line
	O = {0.25, 0.45, 1.00, 0.30},  -- our territory
	e = {1.00, 0.22, 0.18, 0.16},  -- the enemy's, beyond the enemy line
	E = {1.00, 0.22, 0.18, 0.30},  -- enemy territory
	cA = {0.25, 0.45, 1.00, 0.26},  -- contested: blue stripes
	cB = {1.00, 0.22, 0.18, 0.26},  -- and red stripes
}
local STRIPE_CELLS = 3  -- each stripe this many cells wide, on the diagonal
local FILL_LIFT = 4  -- elmos: fills sample the ground only at cell corners, so they float a little over it

local BORDER_COLOUR = {0.85, 0.85, 0.85, 0.35}
local BORDER_WIDTH = 1

local LINES = {  -- by kind, drawn in this order
	{kind = "w", colour = {1.00, 1.00, 0.25, 0.80}, width = 3},  -- forward line
	{kind = "f", colour = {1.00, 1.00, 1.00, 1.00}, width = 14},  -- front line: a halo under its two lines
	{kind = "h", colour = {0.55, 0.05, 0.05, 0.95}, width = 6},  -- enemy-held, off the enemy line
	{kind = "g", colour = {0.60, 0.60, 0.60, 0.95}, width = 6},  -- enemy line, not seen enough to rank
	{kind = "e", colour = {1.00, 0.20, 0.15, 0.95}, width = 6},  -- enemy line, defended
	{kind = "u", colour = {1.00, 0.60, 0.75, 0.95}, width = 6},  -- enemy line, undefended
	{kind = "c", colour = {0.30, 0.55, 1.00, 0.95}, width = 4},  -- current line, towers cover what it wants
	{kind = "s", colour = {1.00, 0.55, 0.05, 0.95}, width = 4},  -- current line, short
}

local LIFT = 10  -- lines float this high over the ground
local STEP = 16  -- elmos between line vertices, so lines follow the ground
local LABEL_SIZE = 40
local LABEL_RANGE = 3500  -- labels farther from the camera than this aren't drawn
local STALE_FRAMES = 30 * 30  -- data the AI hasn't refreshed in 30 s isn't drawn

--------------------------------------------------------------------------------
-- State

local PREFIX = "circuit_sides_"

local spGetGroundHeight = Spring.GetGroundHeight
local spGetGameFrame = Spring.GetGameFrame
local spGetCameraPosition = Spring.GetCameraPosition
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
	DeleteList(team, "lineList")
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

local function BuildLines(team)
	DeleteList(team, "lineList")
	local lines = team.lines or {}
	team.lineList = gl.CreateList(function()
		for _, style in ipairs(LINES) do
			gl.Color(style.colour)
			gl.LineWidth(style.width)
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

function handlers.labels(team, body)
	local labels = {}
	for _, record in ipairs(Records(body)) do
		local x, z, text = record:match("^(%-?%d+) (%-?%d+) (.*)$")
		if x then
			labels[#labels + 1] = {tonumber(x), tonumber(z), text}
		end
	end
	team.labels = labels
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
	if team.lineList then
		gl.CallList(team.lineList)
	end
	gl.DepthMask(true)
	gl.DepthTest(false)
	gl.Color(1, 1, 1, 1)
end

function widget:DrawScreen()
	if ShownTeam() and font then
		font:Print("CircuitAI sides: ally team " .. shown, 16, 16, 14, "o")
	end
end

function widget:DrawWorld()
	local team = ShownTeam()
	if not team or not team.labels or #team.labels == 0 or not font then
		return
	end
	local cx, cy, cz = spGetCameraPosition()
	gl.DepthTest(false)
	font:Begin()
	font:SetTextColor(1, 1, 1, 1)
	font:SetOutlineColor(0, 0, 0, 1)
	for i = 1, #team.labels do
		local label = team.labels[i]
		local x, z = label[1], label[2]
		local y = GroundY(x, z) + 24
		local dx, dy, dz = x - cx, y - cy, z - cz
		if dx * dx + dy * dy + dz * dz <= LABEL_RANGE * LABEL_RANGE then
			gl.PushMatrix()
			gl.Translate(x, y, z)
			gl.Billboard()
			font:Print(label[3], 0, 0, LABEL_SIZE, "con")
			gl.PopMatrix()
		end
	end
	font:End()
	gl.Color(1, 1, 1, 1)
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
