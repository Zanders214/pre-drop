window.BENCHMARK_DATA = {
  "lastUpdate": 1782403237587,
  "repoUrl": "https://github.com/Zanders214/pre-drop",
  "entries": {
    "ZandersPreDrop DSP": [
      {
        "commit": {
          "author": {
            "email": "152227414+Zanders214@users.noreply.github.com",
            "name": "Dennis Zanders",
            "username": "Zanders214"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "93be0f8fd1b0e06af7cefd20c43474476a2d2162",
          "message": "Merge pull request #4 from Zanders214/claude/juce-plugin-ci-setup-tqe6rx",
          "timestamp": "2026-06-25T17:20:25+03:00",
          "tree_id": "84b82e8254670e333f194296253e9af315eea37f",
          "url": "https://github.com/Zanders214/pre-drop/commit/93be0f8fd1b0e06af7cefd20c43474476a2d2162"
        },
        "date": 1782397419454,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 58086.235,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.545,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "152227414+Zanders214@users.noreply.github.com",
            "name": "Dennis Zanders",
            "username": "Zanders214"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "a83b4307c8d7f8332985cdaca2af12db84f355be",
          "message": "Merge pull request #5 from Zanders214/claude/optimize-dsp-hotpath\n\nOptimize DSP hot path: cache filter cutoffs (drop per-sample std::tan)",
          "timestamp": "2026-06-25T18:58:20+03:00",
          "tree_id": "6f04916ea0662605c2b05a70391d862bd98cb47a",
          "url": "https://github.com/Zanders214/pre-drop/commit/a83b4307c8d7f8332985cdaca2af12db84f355be"
        },
        "date": 1782403237182,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 45663.982,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.428,
            "unit": "%"
          }
        ]
      }
    ]
  }
}