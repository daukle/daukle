daukle.plugin{ api = 1, uses = {} }

daukle.resolver{
  resolve = function(coordinate)
    return {
      url = "https://example.invalid/" .. coordinate .. ".lua",
      resolved = coordinate,
      headers = { Authorization = "Bearer secret" },
    }
  end,
}
