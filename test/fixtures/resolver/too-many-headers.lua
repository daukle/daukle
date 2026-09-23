daukle.plugin{ api = 1, uses = {} }

daukle.resolver{
  resolve = function(coordinate)
    local headers = {}
    for index = 1, 17 do headers["X-Header-" .. index] = tostring(index) end
    return {
      url = "https://example.invalid/" .. coordinate .. ".lua",
      resolved = coordinate,
      headers = headers,
    }
  end,
}
