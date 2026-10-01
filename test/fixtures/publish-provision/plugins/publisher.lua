daukle.plugin{ api = 1, uses = { "provision", "exec", "env" } }

daukle.publisher{
  name = "somewhere",
  publish = function(context)
    local root = daukle.provision{
      url    = daukle.env("DAUKLE_TEST_ARCHIVE_URL"),
      sha256 = daukle.env("DAUKLE_TEST_ARCHIVE_SHA256"),
      as     = "test toolchain 1.0",
    }
    daukle.exec(root:tool(daukle.env("DAUKLE_TEST_MEMBER")), { "--task-child" })
  end,
}
