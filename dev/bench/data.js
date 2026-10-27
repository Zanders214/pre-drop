window.BENCHMARK_DATA = {
  "lastUpdate": 1793093403224,
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
          "id": "8e7f4ddc9f81ede72a8b732b1ea88a3d1927a8c8",
          "message": "Merge pull request #6 from Zanders214/codex/automated-releases\n\nAdd automated cross-platform releases",
          "timestamp": "2026-07-14T18:42:31+02:00",
          "tree_id": "6d726284f0eaa36fc47941ce72596233ea8e5cc6",
          "url": "https://github.com/Zanders214/pre-drop/commit/8e7f4ddc9f81ede72a8b732b1ea88a3d1927a8c8"
        },
        "date": 1784047546996,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 41284.881,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.387,
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
          "id": "a664ce50db16aad59d53257009a1a6a4a180f54c",
          "message": "Merge pull request #7 from Zanders214/dev\n\ndev to main",
          "timestamp": "2026-07-14T19:16:43+02:00",
          "tree_id": "6d726284f0eaa36fc47941ce72596233ea8e5cc6",
          "url": "https://github.com/Zanders214/pre-drop/commit/a664ce50db16aad59d53257009a1a6a4a180f54c"
        },
        "date": 1784049552395,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 41923.55,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.393,
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
          "id": "d7a4ae1f3fbd237a5910f312c18708e63ceb9773",
          "message": "Merge pull request #9 from Zanders214/claude/ci-ccache\n\nCI: cache compiles with ccache and drop the duplicate Linux build",
          "timestamp": "2026-10-23T13:33:48+02:00",
          "tree_id": "c5f38133e81f0e9dcfb88961b8a47884750a2ab1",
          "url": "https://github.com/Zanders214/pre-drop/commit/d7a4ae1f3fbd237a5910f312c18708e63ceb9773"
        },
        "date": 1792755685625,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 37030.844,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.347,
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
          "id": "9e59a3348333da633ac5ba67478f7e9601360e79",
          "message": "Merge pull request #8 from Zanders214/claude/riser-audible-earlier\n\nMake the riser noise audible soon after it engages",
          "timestamp": "2026-10-23T13:41:53+02:00",
          "tree_id": "8014b5324d85d9095c046e0e6af9d0c63a48eee0",
          "url": "https://github.com/Zanders214/pre-drop/commit/9e59a3348333da633ac5ba67478f7e9601360e79"
        },
        "date": 1792755778341,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 29811.791,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.279,
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
          "id": "528cebb848501ae1f58346fa5d4efbae791cc45a",
          "message": "Merge pull request #10 from Zanders214/claude/sonar-quality-gate\n\nFix the SonarCloud quality gate and clear all 51 code smells",
          "timestamp": "2026-10-23T14:01:24+02:00",
          "tree_id": "8b9acf49f608e088f1ab18253cb2ec51d3cd3566",
          "url": "https://github.com/Zanders214/pre-drop/commit/528cebb848501ae1f58346fa5d4efbae791cc45a"
        },
        "date": 1792756963178,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 41936.486,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.393,
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
          "id": "66af22fd6676b3b1e8ebc4ceb3f729c457f0cbc2",
          "message": "Merge pull request #11 from Zanders214/claude/sonar-branch-coverage\n\nSonar coverage: count real branch conditions, test the untested ones",
          "timestamp": "2026-10-23T14:20:31+02:00",
          "tree_id": "5874601cd7dd3985a2594ea06e58baa6854548ac",
          "url": "https://github.com/Zanders214/pre-drop/commit/66af22fd6676b3b1e8ebc4ceb3f729c457f0cbc2"
        },
        "date": 1792758408454,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 28708.1,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.269,
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
          "id": "9524a888ef99f37fb1cfdf36219eeca2b50bd676",
          "message": "Merge pull request #12 from Zanders214/claude/effects-in-sync\n\nEffects: rise together from 0% and sit at the same level",
          "timestamp": "2026-10-23T17:11:54+02:00",
          "tree_id": "64ebef7f17135e943f739fbb860a6a5bfb58b22f",
          "url": "https://github.com/Zanders214/pre-drop/commit/9524a888ef99f37fb1cfdf36219eeca2b50bd676"
        },
        "date": 1792768617254,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 41877.532,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.393,
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
          "id": "a106dc8883a693e007b2e839359cf24d1f3483ba",
          "message": "Merge pull request #13 from Zanders214/claude/riser-sample\n\nRiser: drop in your own sample, looped at the calibrated level",
          "timestamp": "2026-10-26T19:15:10+01:00",
          "tree_id": "41fd67449d631d5c6e258e8d8d410a92a8111a7f",
          "url": "https://github.com/Zanders214/pre-drop/commit/a106dc8883a693e007b2e839359cf24d1f3483ba"
        },
        "date": 1793038592245,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 36787.222,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.345,
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
          "id": "f0d03797f4d6a07f94b69931b874d4ff0094642f",
          "message": "Merge pull request #14 from Zanders214/claude/track-linking\n\nLink tracks and returns to a main PreDrop, with per-track effect levels",
          "timestamp": "2026-10-26T20:18:32+01:00",
          "tree_id": "bf2e0dbc3caf89b2c7fedb236f7b774db67889f2",
          "url": "https://github.com/Zanders214/pre-drop/commit/f0d03797f4d6a07f94b69931b874d4ff0094642f"
        },
        "date": 1793042799403,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 36762.24,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.345,
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
          "id": "98092afb75b45217f085e47e417265f626dbaae1",
          "message": "Merge pull request #15 from Zanders214/claude/simpler-linking\n\nSimpler linking: automatic hand-off, auto-TRACK, role menu",
          "timestamp": "2026-10-26T20:53:56+01:00",
          "tree_id": "7c88badc8372f60a460729f893076cba3258369d",
          "url": "https://github.com/Zanders214/pre-drop/commit/98092afb75b45217f085e47e417265f626dbaae1"
        },
        "date": 1793044508397,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 42592.991,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.399,
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
          "id": "9f51b2e328227055689a06d1e43f49425fe5a37c",
          "message": "Merge pull request #16 from Zanders214/claude/plugin-slots\n\nUse your own VST3 as PreDrop's reverb or delay",
          "timestamp": "2026-10-27T01:45:44+01:00",
          "tree_id": "f5aea53bfa5239d6cc84f24c2aac2eca46542934",
          "url": "https://github.com/Zanders214/pre-drop/commit/9f51b2e328227055689a06d1e43f49425fe5a37c"
        },
        "date": 1793062040565,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 42708.196,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.4,
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
          "id": "f24fe16979f0e47abda90c6034e0bf4acc6aa858",
          "message": "Merge pull request #17 from Zanders214/claude/sonar-cleanup\n\nClear the 32 SonarCloud issues",
          "timestamp": "2026-10-27T10:28:52+01:00",
          "tree_id": "4b1ecb6191f80ced35bd1fbb7f426084fb4b305a",
          "url": "https://github.com/Zanders214/pre-drop/commit/f24fe16979f0e47abda90c6034e0bf4acc6aa858"
        },
        "date": 1793093402560,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "processBlock",
            "value": 30548.818,
            "unit": "ns/block"
          },
          {
            "name": "DSP load @48k/512",
            "value": 0.286,
            "unit": "%"
          }
        ]
      }
    ]
  }
}