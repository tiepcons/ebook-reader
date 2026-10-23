# Corpus sources

All files are public-domain texts from Project Gutenberg (https://www.gutenberg.org).
Gutenberg texts are public domain in the US; check local law for other jurisdictions.

| file | format | source URL | license |
|---|---|---|---|
| good/epub/pg1342.epub | EPUB | https://www.gutenberg.org/ebooks/1342.epub.noimages | Public domain (Project Gutenberg, Pride and Prejudice, Austen) |
| good/epub/pg11.epub | EPUB | https://www.gutenberg.org/ebooks/11.epub3.images | Public domain (Project Gutenberg, Alice's Adventures in Wonderland) |
| good/epub/pg84.epub | EPUB | https://www.gutenberg.org/ebooks/84.epub3.images | Public domain (Project Gutenberg, Frankenstein, Shelley) |
| good/txt/pg1342.txt | TXT (UTF-8) | https://www.gutenberg.org/cache/epub/1342/pg1342.txt | Public domain (Project Gutenberg, Pride and Prejudice) |
| good/txt/pg11.txt | TXT (UTF-8) | https://www.gutenberg.org/cache/epub/11/pg11.txt | Public domain (Project Gutenberg, Alice's Adventures in Wonderland) |
| good/txt/pg84.txt | TXT (UTF-8) | https://www.gutenberg.org/cache/epub/84/pg84.txt | Public domain (Project Gutenberg, Frankenstein) |
| good/mobi/pg11.mobi | MOBI (Kindle) | https://www.gutenberg.org/ebooks/11.kindle.images | Public domain (Project Gutenberg, Alice's Adventures in Wonderland) |
| good/mobi/pg84.mobi | MOBI (Kindle) | https://www.gutenberg.org/ebooks/84.kindle.images | Public domain (Project Gutenberg, Frankenstein) |

## Notes
- Pride and Prejudice EPUB: the images variant (24.8 MB) exceeds the 5 MB limit, so the no-images EPUB is used.
- Pride and Prejudice MOBI: Kindle variant is 25 MB, over the 5 MB limit, skipped.
- AZW3 / KF8: not fetched (not probed; Kindle .kindle.images variant used instead).
- Vietnamese (language=vi) on Project Gutenberg: none on Gutenberg (search page returns zero results).
- PDF, FB2, CBZ and corrupted samples are not stored here; they will be generated later with mutool.

## Added in T0.10 (part 2)

Vietnamese text and generated samples. Source for all text: Wikisource (public domain).

| file | format | source URL | license |
|---|---|---|---|
| good/txt/truyen-kieu-utf8.txt | TXT (UTF-8, no BOM) | https://vi.wikisource.org/wiki/Truy%E1%BB%87n_Ki%E1%BB%81u (raw: https://vi.wikisource.org/w/index.php?title=Truy%E1%BB%87n_Ki%E1%BB%81u&action=raw) | Public domain (Nguyen Du, d. 1820; Wikisource transcription). Wiki markup stripped. |
| good/txt/truyen-kieu-utf8bom.txt | TXT (UTF-8 with BOM) | same as above | same; same text as utf8.txt with BOM |
| good/txt/truyen-kieu-utf16le.txt | TXT (UTF-16 LE with BOM) | same as above | same; same text re-encoded |
| good/txt/truyen-kieu-cp1258.txt | TXT (Windows-1258) | same as above | same; LOSSY: characters not representable in cp1258 were replaced by '?' during encoding |
| good/fb2/sample-en.fb2 | FB2 2.0 (UTF-8) | body: good/txt/pg11.txt (https://www.gutenberg.org/cache/epub/11/pg11.txt) | Public domain (Lewis Carroll). First 300 lines after Gutenberg header. Cover PNG generated locally (System.Drawing). |
| good/fb2/sample-vi.fb2 | FB2 2.0 (UTF-8) | body: good/txt/truyen-kieu-utf8.txt | Public domain (Nguyen Du). First 300 lines. Cover PNG generated locally. |
| good/cbz/sample.cbz | CBZ (ZIP of 5 PNG) | generated locally (page01.png..page05.png, 800x1200, synthetic) | Generated for this project; no third-party content |
| good/pdf/ | PDF | PDF pending: govinfo.gov candidates checked. CDOC-110hdoc50 is 4.0 MB (over 2 MB limit); GPO-CONAN-1992 and GPO-CONSTITUTION-2017-1 URLs return HTML, not PDF; senate.gov returned 403; archives.gov 404. | - |
| bad/truncated.epub | corrupt EPUB | first 30% of good/epub/pg11.epub | derived from Gutenberg pg11 |
| bad/garbage.pdf | corrupt PDF | '%PDF-1.7' header + random bytes (4095 bytes total) | generated locally |
| bad/zipbomb-ish.cbz | ZIP bomb-ish CBZ | single entry page.png, 50 MB of zeros, deflated (51084 bytes total) | generated locally |
| bad/empty.epub | empty file | 0 bytes | generated locally |
| bad/notzip.cbz | text file with .cbz extension | plain text, 67 bytes | generated locally |
| bad/badxml.fb2 | malformed FB2 | unclosed tags, 287 bytes | generated locally |
| bad/truncated.mobi | corrupt MOBI | first 10 KB of good/mobi/pg11.mobi | derived from Gutenberg pg11 |
| good/pdf/us-constitution-cdoc-110hdoc50.pdf | PDF | https://www.govinfo.gov/content/pkg/CDOC-110hdoc50/pdf/CDOC-110hdoc50.pdf | Public domain (U.S. Government work, 17 U.S.C. 105) |
| good/azw3/pg84-kf8.azw3 | AZW3 (KF8) | https://www.gutenberg.org/cache/epub/84/pg84-images-kf8.mobi | Public domain (Project Gutenberg) |
