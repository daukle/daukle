daukle.plugin{ api = 1, uses = { "region" } }

daukle.language{
  name = "c",
  apply = function(consumer, resolved, text)
    if consumer.configuration == nil or consumer.configuration == "" then
      error("consumer has no \"configuration\" for the c language plugin")
    end

    local body = {}
    if #resolved > 0 then
      body[#body + 1] = "include(FetchContent)\n"
      local links = {}
      for index = 1, #resolved do
        local entry = resolved[index]
        local package = entry.block.package
        local url = entry.block.url
        if type(package) ~= "string" or type(url) ~= "string" then
          error("modules." .. entry.module .. ".c needs \"package\" and \"url\"")
        end
        local declare = "FetchContent_Declare(" .. package .. " URL \"" .. url .. "\""
        if type(entry.block.sha256) == "string" then
          declare = declare .. " URL_HASH SHA256=" .. entry.block.sha256
        end
        body[#body + 1] = declare .. ")\n"
        body[#body + 1] = "FetchContent_MakeAvailable(" .. package .. ")\n"
        links[index] = " " .. package
      end
      body[#body + 1] = "target_link_libraries(" .. consumer.configuration .. " PRIVATE"
                        .. table.concat(links) .. ")"
    end

    return daukle.region(text, "# daukle:begin", "# daukle:end", table.concat(body))
  end,
}
