# Pinned inputs of the optional VieNeu pack (tools/package.ps1). Everything is downloaded once into build/downloads and
# verified against these SHA256 values; the files are only extracted or copied, never executed.
@{
    AudioCpp = @{
        Url    = 'https://github.com/0xShug0/audio.cpp/releases/download/v0.9.3/audio-v0.9.3-bin-windows-x64-cpu.zip'
        Sha256 = '3ac66f46395b4e92e4f3bb68b200f0032310bd9922f12d97f10ccdadb02dee4b'
        Files  = @('audiocpp_cli.exe', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vcomp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll')
    }
    SeaG2pDll = @{
        Url    = 'https://github.com/pnnbao97/sea-g2p/releases/download/v0.10.0/sea_g2p_rs-windows-x86_64.dll'
        Sha256 = '1daff10cf65304daa56d90814f4f6a54cf6269bf22a684b940a3d7c0bbea25c3'
    }
    SeaG2pDict = @{
        Url    = 'https://github.com/pnnbao97/sea-g2p/releases/download/v0.10.0/sea_g2p.bin'
        Sha256 = '4346e690d0711ebc5231e7a42c5c88aaf6e40377e894b4617c018fd81c6f4096'
        Size   = 62829820
    }
    Model = @{
        Url    = 'https://huggingface.co/pnnbao-ump/VieNeu-TTS-v3-Turbo/resolve/61b85e3d937fbbacb387714180e8182823512523/gguf/vieneu-v3-turbo-q8_0.gguf'
        Sha256 = 'a60569e5d7dd6f24cbb7bb2e7c472bb988ad19563a4e78f0cc67cc53fd189bb4'
        Size   = 187917664
    }
    Voices = @{
        BaseUrl  = 'https://huggingface.co/pnnbao-ump/VieNeu-TTS-v3-Turbo/resolve/61b85e3d937fbbacb387714180e8182823512523/gguf/voices'
        Manifest = 'c11561e2b31636c1cd6d7244894342d37e03283171f9cfc4bd9fbb782debda7e'
        Files    = @{
            'adam_bua/speaker.emb.txt' = '857cc3c5efac88ccad9424c381749a89db1944f7825486779d51001671f2634f'
            'adam_bua/ref_codes.txt' = '31689128ddb421780ee146e6c0df5e8d0b54e6b370042f721bb816ac1d55f1a2'
            'truc_ly/speaker.emb.txt' = 'eb918bb627224ae35385cd129324c2e3bd393dfea9ffe0cf732bf5b8667bd433'
            'truc_ly/ref_codes.txt' = '8d4fc8795ae19bf23f86fa2ed26843ebb0d6ec14a6a4488a821d951766ee74e5'
            'anh_khoi/speaker.emb.txt' = '37712a86e25ba1301051a23cdf8eab06097b549c3005d8d4bc9a7702202bb55c'
            'anh_khoi/ref_codes.txt' = 'd5f29595ee58520b3b2ed500326ad7c4a1c673a9ec6a40e2c6a51bce2e94f08a'
            'mai_anh/speaker.emb.txt' = '675f85a6cbd97efdf8fdc14cd42b81b519781926885a98e8e9eb8d5343488781'
            'mai_anh/ref_codes.txt' = '3821233d371217dbde7953d0a477bce826c2a536e69a4b64059a54b0fb5f3ab9'
            'minh_quan_pro/speaker.emb.txt' = 'f7bdb7235f5e3c14f9a45e60da8700df9b05eb6f880adf1cc143458c3c12b281'
            'minh_quan_pro/ref_codes.txt' = '3e4e64aaaf657e68e906e16fdb7955e596f5b4ff94ef77dbd2313469312e5b2d'
            'thuy_dung/speaker.emb.txt' = '9bbb198a7eb0166913d0c3369729dc440ca010e118590fce01640ff3b70bb3ee'
            'thuy_dung/ref_codes.txt' = '794a29bd068791315eb5855422135ed5e35fbe6c57f6fe257142b42633cb15aa'
            'thien_tam_duc/speaker.emb.txt' = '75eb3563bb0bd59ba867c81bfa4bd2ee1ceaa6ea562c9294e234f51e8a977b4e'
            'thien_tam_duc/ref_codes.txt' = '3ecf6314ae2c75df07d69f0e3fa390620c6302caf4253dd38ac678bd063d5979'
            'ngoc_huyen/speaker.emb.txt' = '648be3a2c36191cf35c0234d7c2089aa64cdd2ff4714e5774f1d92b2399b5a9d'
            'ngoc_huyen/ref_codes.txt' = '6798a6cb4ec607d1403ae249a711bda931eb8d3049bdec26f8a758be42a574e4'
            'quang_son/speaker.emb.txt' = 'e7b702c59974a5eae4a438ea03feedf5bb184b27f5b25b7e853f645cd127e28c'
            'quang_son/ref_codes.txt' = '52b29d11b493dedf4502d3754b004bdda247e00e91997f24929d6609d3b2fece'
            'ngoc_tran/speaker.emb.txt' = 'eb3c7956ffc73c9b9f67e507aedf35fd9562d7b8d0ba8caa347367139f06ff9c'
            'ngoc_tran/ref_codes.txt' = 'ec7356e9a9ec8976fe1b622acc596d07754a01e95a41f94ef7a3554432d7dd29'
            'minh_duc/speaker.emb.txt' = '308923f826c1471cc3a8118968c520c5813ed5c9ef3401659a5d9d03e5335b4b'
            'minh_duc/ref_codes.txt' = '2c1fec0bc3d151a71335b2ea465b2c57e0a902eabf2456456337f3fd596f9fa2'
            'pham_tuyen/speaker.emb.txt' = 'ab1d8fb2de077d233327df815458c535e884408de13ad83ef0a5f860a6ba0b14'
            'pham_tuyen/ref_codes.txt' = 'e08e6ade6d79f14f365b9d6b0f5e976e4c1eda0327b51e77c11423f250788865'
            'thai_son/speaker.emb.txt' = '3349e8a1bda30f29ffbb7cb8f7c08c8408778e1739c9a0f3fc9bac4acf30e748'
            'thai_son/ref_codes.txt' = '63aa0153fe57ce4e18500cef90fbb0dd80d74dd478627bde4294b4d91b31f94d'
            'xuan_vinh/speaker.emb.txt' = '6894bdd8957297c9069e9ae1a0473518af20c91f4ea37d072332c62415e668bb'
            'xuan_vinh/ref_codes.txt' = '0ac59a700170192d25caca083464d408efac0090febdf0ef81dffe9f1ef3d052'
            'thanh_binh/speaker.emb.txt' = 'df8e0cdc1b6b438ea97771d79893a486ca80c01ea7fb1416610f8d16003e021d'
            'thanh_binh/ref_codes.txt' = 'b43c63e1c703718dc62477fb32b752e0748e272d725dc3997855a864fd107f25'
            'ngoc_linh/speaker.emb.txt' = '05615861f5f2a2486f27f5a496a411b70c237b67f60b3641051e0763057cc34e'
            'ngoc_linh/ref_codes.txt' = '14b7a39788e6c06196ff272123713f4d7a4684dd9439f609036481e1fd41acff'
            'doan_trang/speaker.emb.txt' = '4307777ca15abfaa8954dc20a5a3320dbac314644b3c73a528721c0feca0d8b6'
            'doan_trang/ref_codes.txt' = '001ca36e9d4b8aa73c81e7f8a5e0f88750d2684e861035af8a0fe743e9abaeaa'
            'thuc_doan/speaker.emb.txt' = 'aae1818cda77c25d6ccd39c64387695b895fa5e85c7b279205fea744fb95a400'
            'thuc_doan/ref_codes.txt' = '9d084951e34f7c2d3cf7c6bb0f2fbcdc6e61e1e369bb0dfed1c2874077ca6bad'
            'minh_triet/speaker.emb.txt' = 'b9a5fbefa992a012f3fc8d898aebda5248aef9cd7984d1a28bd0cf273a882537'
            'minh_triet/ref_codes.txt' = '301649bbc47e2276392091055b42967928541453550013d4ada494284797c617'
            'my_duyen/speaker.emb.txt' = 'a4d6fb831600f6f819fb92e138000d59335bac644662747487a0e36811388a40'
            'my_duyen/ref_codes.txt' = '2e60ac6335aebc9f948cbf8b905d9625f5ef9a268b45be41e8cb47e887d781a5'
            'quynh_anh/speaker.emb.txt' = 'ab66bc624b0ffaa735c8ece1aa71b12f18b48bddc2e7cd27ba9eec3d85c2bce9'
            'quynh_anh/ref_codes.txt' = '363b2eee93aeca4f789e1ce63ad904d7e0bef88bb20c830dd2e559ea3fe0054a'
            'duc_tri/speaker.emb.txt' = 'ae7ac1ce851c516ab7cbf1b2dcf6d1f217d845ad8d89e67f6884c8f4ae2b2ece'
            'duc_tri/ref_codes.txt' = 'a7ca96c6852e5c54189e35712bef8874e6fa7db9ef40d2b35415e37a96ed1627'
            'kim_thanh/speaker.emb.txt' = '88fcba766014f1fccdc1fdd70410dafc39a873efdbb489fe31838e2f605598ff'
            'kim_thanh/ref_codes.txt' = 'f8eaa7d25a67a804639d679e84d704474e6cd56128f35c542c17f9c44a58cc95'
            'adam/speaker.emb.txt' = 'c5c036de55f8b3d85e457cded1194a4a34b1fe3cb58ffefd7a068588ddac359c'
            'adam/ref_codes.txt' = 'fcbdcd6208768abfe843dd2be76ddaab41439319063c57a7f7337e7f1bf0ae31'
            'manh_dung/speaker.emb.txt' = '6f80acfe4fff4eb53e5ce3a1f58f2d39303669250d1a8d621019d88679bec357'
            'manh_dung/ref_codes.txt' = '5fd0ea71fe3ac0d1eff233324f603be920f50e88bcb9240e48c3040b8ed03b9a'
        }
    }
    Licenses = @{
        'LICENSE-ggml-MIT.txt' = @('https://raw.githubusercontent.com/0xShug0/audio.cpp/v0.9.3/external/ggml/LICENSE', 'bcd8ec749126d45cb06737d0690295d73df4b6e7e194205bcf91190368f27285')
        'LICENSE-cJSON-MIT.txt' = @('https://raw.githubusercontent.com/0xShug0/audio.cpp/v0.9.3/external/cJSON/LICENSE', 'a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c')
        'LICENSE-libyaml-MIT.txt' = @('https://raw.githubusercontent.com/0xShug0/audio.cpp/v0.9.3/external/libyaml/License', 'c40112449f254b9753045925248313e9270efa36d226b22d82d4cc6c43c57f29')
        'LICENSE-sentencepiece-Apache-2.0.txt' = @('https://raw.githubusercontent.com/0xShug0/audio.cpp/v0.9.3/external/sentencepiece/LICENSE', '3ddf9be5c28fe27dad143a5dc76eea25222ad1dd68934a047064e56ed2fa40c5')
        'LICENSE-utf8proc.txt' = @('https://raw.githubusercontent.com/0xShug0/audio.cpp/v0.9.3/external/utf8proc/LICENSE.md', '3b510150d34f248a221bb88e1d811238d6c6c18b51231822c42974c39bb07256')
        'LICENSE-sea-g2p-Apache-2.0.txt' = @('https://raw.githubusercontent.com/pnnbao97/sea-g2p/v0.10.0/LICENSE', 'c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4')
    }
}
