@{
    # Single source of truth for the Piper runtime built by tools/build_piper.ps1 and described by tools/package.ps1.
    # Sources are fetched by full commit SHA (git verifies content); archives are verified by SHA256. No floating refs.
    Sources = @{
        piper    = @{ Url = 'https://github.com/rhasspy/piper';          Commit = '38917ffd8c0e219c6581d73e07b30ef1d572fce1'; Ref = 'tag 2023.11.14-2' }
        phonemize = @{ Url = 'https://github.com/rhasspy/piper-phonemize'; Commit = 'fccd4f335aa68ac0b72600822f34d84363daa2bf'; Ref = 'tag 2023.11.14-4' }
        espeak   = @{ Url = 'https://github.com/rhasspy/espeak-ng';      Commit = '0f65aa301e0d6bae5e172cc74197d32a6182200f'; Ref = 'rhasspy fork master, 2023-09-13' }
        fmt      = @{ Url = 'https://github.com/fmtlib/fmt';             Commit = 'a0b8a92e3d1532361c2f7feb63babc5c18d00ef2'; Ref = 'tag 10.0.0' }
        spdlog   = @{ Url = 'https://github.com/gabime/spdlog';          Commit = '7e635fca68d014934b4af8a1cf874f63989352b7'; Ref = 'tag v1.12.0' }
    }
    Onnxruntime = @{
        Url    = 'https://github.com/microsoft/onnxruntime/releases/download/v1.14.1/onnxruntime-win-x64-1.14.1.zip'
        Sha256 = '1243f2c505de8845b6927ae1cbde75b9372f16965c6b2c8db2279f385003fd85'
        Dir    = 'onnxruntime-win-x64-1.14.1'
    }
    # libtashkeel_model.ort ships in piper-phonemize at etc/libtashkeel_model.ort (pinned commit above); this is its licence.
    TashkeelLicense = @{
        Url    = 'https://raw.githubusercontent.com/mush42/libtashkeel/9b91c2dbc3dc4085dc4f77a9e250e5dd095d7d0c/LICENSE'
        Sha256 = '98dbc9a6a01badb391f2b64161742cb9cd8fcdc5cfc97486be0d49c47e98d1a8'
    }
}
