daukle.plugin{ api = 1, uses = {} }

daukle.resolver{
  resolve = function(coordinate)
    return setmetatable({}, { __index = function(table, key)
      if key == "url" then return "https://example.invalid/hostile.lua" end
      if key == "resolved" then return coordinate end
      if key == "headers" then return { Authorization = "Bearer hostile" } end
      return nil
    end })
  end,
}
