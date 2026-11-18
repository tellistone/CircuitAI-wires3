--------------------------------------------------------------------------------
-- CircuitAI sides: draws the choke_overlay "sides" layer that CircuitAI sends.
--
-- Install: copy this file into your Zero-K data folder's LuaUI/Widgets/ (local widgets are on by default), then in
-- a game against CircuitAI say "choke_overlay sides" in ally chat. It only draws in games with no human opponents,
-- on the machine that runs the AI.
--
-- Regions behind our current line are tinted blue, beyond the enemy line red, stronger where that side has
-- buildings. Contested regions (between the lines, both armies there in the last 3 minutes, or both sides' buildings)
-- are striped blue and red. Dashed lines are region borders. Choke lines: blue current, red enemy, white front,
-- yellow forward. Orange arrows run from enemy armies to the line chokes they press, thick to the nearest.
--
-- Messages (CChokeOverlay::SendWidgetData in src/circuit/terrain/ChokeSides.cpp), "circuit_sides_<verb>:<ally team>"
-- then a body: hello, grid, state, lines, arrows, labels, clear.
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

local BORDER_COLOUR = {0.85, 0.85, 0.85, 0.45}
local BORDER_WIDTH = 1.5
local DASH_ON, DASH_OFF = 48, 32  -- elmos

local LINES = {  -- by kind, drawn in this order
	{kind = "w", colour = {1.00, 1.00, 0.25, 0.80}, width = 3},  -- forward line
	{kind = "c", colour = {0.30, 0.55, 1.00, 0.95}, width = 6},  -- current line
	{kind = "e", colour = {1.00, 0.25, 0.20, 0.95}, width = 6},  -- enemy line
	{kind = "f", colour = {1.00, 1.00, 1.00, 1.00}, width = 9},  -- front line: on both
}

local ARROW_FULL = {1.00, 0.60, 0.10, 0.90}
local ARROW_HALF = {1.00, 0.60, 0.10, 0.45}
local ARROW_HEAD = 80  -- elmos

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
local shown = nil  -- the ally team whose AI said hello last
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
	DeleteList(team, "fillList")
	DeleteList(team, "borderList")
	DeleteList(team, "lineList")
	DeleteList(team, "arrowList")
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

local function BuildFills(team)
	DeleteList(team, "fillList")
	if not team.regionOf or not team.state then
		return
	end
	local width, height, cell = team.width, team.height, team.cell
	local regionOf, state = team.regionOf, team.state
	local letters = {}
	for r = 1, #state do
		letters[r - 1] = state:sub(r, r)
	end
	-- Runs of cells with the same fill, row by row, grouped by fill so each colour is set once
	local runs = {}
	for key in pairs(FILL) do
		runs[key] = {}
	end
	local function keyAt(x, z)
		local region = regionOf[z * width + x + 1]
		local letter = region and region >= 0 and letters[region]
		if letter == "c" or letter == "C" then
			return (floor((x + z) / STRIPE_CELLS) % 2 == 0) and "cA" or "cB"
		end
		return FILL[letter] and letter or nil
	end
	for z = 0, height - 1 do
		local start, current = 0, keyAt(0, z)
		for x = 1, width do
			local key = (x < width) and keyAt(x, z) or nil
			if key ~= current then
				if current then
					local list = runs[current]
					list[#list + 1] = {start * cell, z * cell, x * cell, (z + 1) * cell}
				end
				start, current = x, key
			end
		end
	end
	team.fillList = gl.CreateList(function()
		for key, list in pairs(runs) do
			if #list > 0 then
				gl.Color(FILL[key])
				for i = 1, #list do
					local q = list[i]
					gl.DrawGroundQuad(q[1], q[2], q[3], q[4])
				end
			end
		end
	end)
end

-- Dashed lines along every cell edge between two regions (walls get none)
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
				local dx, dz = s[3] - s[1], s[4] - s[2]
				local length = sqrt(dx * dx + dz * dz)
				local ux, uz = dx / length, dz / length
				local at = 0
				while at < length do
					local stop = min(at + DASH_ON, length)
					GroundSegment(s[1] + ux * at, s[2] + uz * at, s[1] + ux * stop, s[2] + uz * stop)
					at = stop + DASH_OFF
				end
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

local function BuildArrows(team)
	DeleteList(team, "arrowList")
	local arrows = team.arrows or {}
	local maxFull = 0
	for i = 1, #arrows do
		maxFull = max(maxFull, arrows[i][5])
	end
	team.arrowList = gl.CreateList(function()
		for i = 1, #arrows do
			local a = arrows[i]
			local x1, z1, x2, z2, full = a[1], a[2], a[3], a[4], a[5]
			local dx, dz = x2 - x1, z2 - z1
			local length = sqrt(dx * dx + dz * dz)
			if length > 1 then
				local ux, uz = dx / length, dz / length
				local head = min(ARROW_HEAD, length * 0.5)
				if full > 0 then
					gl.Color(ARROW_FULL)
					gl.LineWidth(2 + 6 * full / max(maxFull, 1))
				else
					gl.Color(ARROW_HALF)
					gl.LineWidth(2)
				end
				gl.BeginEnd(GL.LINES, function()
					GroundSegment(x1, z1, x2, z2)
					GroundSegment(x2, z2, x2 - (ux + uz * 0.5) * head, z2 - (uz - ux * 0.5) * head)
					GroundSegment(x2, z2, x2 - (ux - uz * 0.5) * head, z2 - (uz + ux * 0.5) * head)
				end)
			end
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
	BuildFills(team)
end

function handlers.state(team, body)
	local state = body:sub(2)
	if state ~= team.state or not team.fillList then
		team.state = state
		BuildFills(team)
	end
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

function handlers.arrows(team, body)
	local arrows = {}
	for _, record in ipairs(Records(body)) do
		local n = Numbers(record)
		if #n >= 6 then
			arrows[#arrows + 1] = n
		end
	end
	team.arrows = arrows
	BuildArrows(team)
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
		if shown == ally then
			shown = nil
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
	if verb == "hello" then
		shown = ally
	end
	team.frame = spGetGameFrame()
	return handler(team, body) or "1"
end

--------------------------------------------------------------------------------
-- Drawing

local function ShownTeam()
	local team = shown and teams[shown]
	if team and (spGetGameFrame() - (team.frame or 0) <= STALE_FRAMES) then
		return team
	end
end

function widget:DrawWorldPreUnit()
	local team = ShownTeam()
	if not team then
		return
	end
	gl.Blending(GL.SRC_ALPHA, GL.ONE_MINUS_SRC_ALPHA)
	gl.DepthTest(true)
	gl.DepthMask(false)
	if team.fillList then
		gl.PolygonOffset(-25, -2)
		gl.CallList(team.fillList)
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
	if team.arrowList then
		gl.CallList(team.arrowList)
	end
	gl.DepthMask(true)
	gl.DepthTest(false)
	gl.Color(1, 1, 1, 1)
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
