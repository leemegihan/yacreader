# 로컬 페이지 OCR로 작가·제목 확인

## 사용 흐름

처음 교체할 때는 라이브러리 이름을 우클릭해 `Back up library database`로
DB를 백업하고, YACReader와 YACReaderLibrary를 모두 종료한 뒤 새 설치 파일을
실행한다. 기존 프로그램이나 만화 원본을 먼저 삭제할 필요는 없다.

1. 이미지 폴더가 있다면 라이브러리를 업데이트(재스캔)한다.
2. 왼쪽 `Unidentified`에서 작품을 선택하고 `앞·뒤 페이지 / OCR…`를 누른다.
3. 기본 앞 3장·뒤 3장의 미리보기를 확인한다. 필요하면 각 1~6장으로 바꾼다.
4. 언어와 모델을 선택하고 `페이지 글자 읽기`를 누른다. 기본값은 페이지별 자동
   언어 비교와 정밀 모델이다. 일본어+영어, 한국어+영어를 각각 읽고 글자 종류와
   OCR 신뢰도로 선택하므로 한 번만 읽는 수동 언어보다 느리다. 불확실한 판정은 표시한다.
   판권 라벨이 있거나 인식 신뢰도가 낮으면 글자 블록 방식도 비교한다.
   특히 한글 음절이 조각으로 나뉘는 경우를 줄이기 위한 재판독이다.
   세로 일본어 판권은 `일본어 세로쓰기`로 다시 읽어볼 수 있다.
   작은 작가명이나 판권란은 이미지에서 드래그하고 `선택 영역 다시 읽기`를
   누른다. 글자 블록/한 줄, 회전, 흰 글자 반전, 불균일한 배경 보정을 선택할 수 있다.
5. 후보를 두 번 클릭해 입력칸에 옮기거나 OCR 원문을 보고 직접 수정한다.
   해당 후보의 근거 페이지도 함께 선택된다.
   자동으로 외부 대조 화면이 열렸다면 그 화면의 후보를 확인하고 Apply로 적용한다.
6. `확인한 정보 저장`을 누른다. 기본적으로 빈 제목·작가만 채우며,
   기존 값을 바꾸려면 덮어쓰기를 명시적으로 선택한다.

명확한 역할 라벨이 있고 후보가 하나인 경우 제목·작가 검토 칸을 미리 채운다.
이는 확정이나 저장이 아니다. 이름 힌트, 라벨 없는 문구, 여러 작가 후보는 자동 선택하지 않는다.

명확한 판권/제목 후보를 찾으면 기본으로 외부 대조 화면을 열고 E-Hentai를 조회한다.
OCR 화면의 자동 조회 체크를 끄면 로컬 확인 화면에 머문다. `메타데이터 대조…`도
현재 제목·작가와 파일명 이름 힌트로 조회를 시작한다. 페이지 수와 발행자 후보는
로컬 대조에만 사용한다. E-Hentai는
제목/작가 검색 결과의 최대 12개 작품 식별자를 메타데이터 API로 대조한다.
알고 있는 작품 링크를 제목 칸에 입력하면 해당 식별자의 메타데이터를 바로 조회한다.
AniList 제목 검색도 선택할 수 있다. 이미지, 원본 경로, 전체 OCR 원문은 전송하지 않는다.
작가 `artist:`와 서클 `group:`를 구분하고 태그의 네임스페이스를 유지한다.
제목 검색이 명시적으로 결과 없음이면 최대 두 이름 힌트를 5초 간격으로 조회한다.
일본어와 영문 필명이 같은 사람이라고 추측해서 작가를 저장하지 않는다.
제목과 작가 힌트가 강하게 일치하고, 권수/페이지 수 충돌과 비슷한 다른 후보가
없을 때만 검토 화면에 자동 입력한다. 그 외에는 후보를 직접 선택한다.
어느 경우든 `Apply`를 눌러야 DB에 저장한다.
제목 유사도는 정답 확률이 아니다. 작가 표기 및 페이지 수 차이도 함께 확인한다.

로그인 필요, 요청 제한, 접근 차단, 검색 화면 형식 변경은 오류/결과 없음으로 처리하며
우회나 브라우저 인증정보 수집을 하지 않는다. ExHentai 링크도 공개 API에서 조회 가능한
정보만 가져올 수 있다. Hitomi와 ExHentai 로그인 세션 기반 조회는 아직 지원하지 않는다.
검색 결과가 없어도 로컬 OCR 후보 저장은 가능하다.

## 지원 범위와 안전장치

- ZIP/CBZ 및 기존 압축 백엔드가 읽을 수 있는 이미지 압축파일.
- 직접 이미지 파일을 담고 있는 단일 작품 폴더. 여러 하위 폴더나 다른
  압축작품이 함께 있는 폴더는 한 권으로 합치지 않는다.
- 이미지 폴더를 라이브러리 작품으로 등록하고, 기존 썸네일·뷰어·메타데이터
  저장 경로를 사용한다. 파일 이동/복사나 자동 리네이밍은 하지 않는다.
- 앞·뒤 범위가 겹치는 짧은 작품은 같은 페이지를 중복 처리하지 않는다.
- 화면을 닫거나 작품/라이브러리를 바꾸면 작업 취소 및 이전 결과 무시.
- OCR 임시 이미지는 임시 폴더에서 처리 후 삭제한다. 이미지 전송/API 요금 없음.
  Windows 설치 경로에 한글 등이 있으면 선택한 언어 데이터도 임시 폴더로
  복사해 엔진의 경로 인코딩 제약을 피하며, 처리가 끝나면 함께 삭제한다.
  일본어 모델이 내부에서 함께 읽는 `jpn_vert`도 임시 복사에 포함한다.
- 저장은 트랜잭션으로 수행한다. 제목·작가 외의 태그·읽음·읽던 위치는 유지한다.
- 검토한 후보·근거 페이지 번호는 DB의 `local_metadata_evidence`에 기록한다.
  원본 이미지나 폴더 안에 메타데이터 파일을 쓰지 않는다.
- 외부 조회 후 적용한 후보는 `catalog_metadata_evidence`에 출처와 함께 기록한다.
  출처 기록에 실패하면 같은 트랜잭션의 메타데이터 변경도 취소한다.

## 인식의 한계

이 단계는 로컬 OCR와 규칙 기반 후보 추출이며, 그림을 이해하는 생성형 AI가 아니다.
작가/저자/著者/作者/Author 등의 크레딧과 제목 라벨을 후보로 삼는다.
`タ イ ト ル`처럼 OCR이 넣은 공백을 정리하고 다음 줄의 값을 연결한다.
판권/후기/속표지 분류는 읽힌 라벨을 이용하는 규칙이며 그림 자체의 분류가 아니다.
`発行者`/서클은 별도 후보로 표시하고 개인 작가 칸에는 넣지 않는다.
번역자·인쇄소·연락처·감사 문구는 작가 후보에서 제외한다.
라벨 없는 본문 문구는 자동 제목 후보로 넣지 않는다. 이름 힌트는 사이트명일 수도 있다.
필체·장식 글씨·저해상도·번역본·누락된 판권에서는 정확하게 찾지 못할 수 있다.
따라서 자동 일괄 확정은 하지 않는다.

PDF/EPUB 고유 페이지 순서, 복잡한 중첩 이미지 폴더, 별명/번역명 통합,
대량 백그라운드 큐 및 추가 메타데이터 사이트는 후속 과제다.
이미지 폴더의 서버 다운로드/파일 정리 기능은 이번 검증 범위가 아니다.

## OCR 배포 및 검증

Windows 워크플로가 vcpkg `2025.06.13`의 Tesseract 5.5.1을 소스 빌드하고,
`tessdata_fast`와 `tessdata_best` 4.1.0의 eng/jpn/jpn_vert/kor를 설치 파일에 포함한다.
페이지 해상도 상한은 긴 변 4,000픽셀이며, 작은 선택 영역은 최대 2배로 확대한 뒤
흰 여백을 붙인다. 정밀 모델은 느리며 실제 작품에서의 정확도 향상은 별도 확인이 필요하다.
엔진 DLL은 앱의 `ocr` 하위 폴더에 격리하고 의존 라이선스도 함께 배포한다.
검증된 Windows 설치본은 별도 OCR 설치나 언어 다운로드를 요구하지 않는다.

- Tesseract: https://github.com/tesseract-ocr/tesseract
- 언어 데이터: https://github.com/tesseract-ocr/tessdata_fast/tree/4.1.0
- 정밀 모델: https://github.com/tesseract-ocr/tessdata_best/tree/e2aad9b983032bb1beff9133104a67cdbb87ca4d
- 전처리 근거: https://tesseract-ocr.github.io/tessdoc/ImproveQuality.html
- OCR 줄/단어 신뢰도 형식: https://tesseract-ocr.github.io/tessdoc/Command-Line-Usage.html#tsv-output
- 메타데이터 형식: https://ehwiki.org/wiki/API
- 검색 문법: https://ehwiki.org/wiki/Gallery_Searching
- vcpkg 패키지: https://github.com/microsoft/vcpkg/tree/2025.06.13/ports/tesseract

합성 표지로 실제 OCR, 폴더/압축 페이지 순서, 짧은 작품 중복 방지,
오류/시간제한/취소, DB 보존, 폴더 등록·재스캔·페이지 추가를 검사한다.
Windows 설치 후 한글·공백 경로에서도 OCR 인식과 앱 시작을 추가 검사한다.
실제 작품에 대한 인식률이나 사용자 PC의 실행 결과로 오해하면 안 된다.

이전 설치본 검증: 2026-09-06 Windows 실행 `34029267025`에서 전체 12개 CTest 묶음,
설치, 설치된 엔진의 글자 인식, 앱의 OCR 호출 함수(한글 설치 경로),
YACReader/YACReaderLibrary 창 시작이 모두 통과했다.
설치 파일은 `YACReader-v10.3.0.13-winx64-7z-qt6.exe`이며, 소스 커밋은
`d85ec8e139096944143ab8e1bd91e5ca7355bc73`이다.

정밀 OCR·외부 대조 설치본 검증: Windows 실행 `34033636584`에서 전체 12개
CTest 묶음과 설치 검증을 통과했다. 로컬 OCR 테스트는 실제 엔진을 포함해
15 passed / 0 failed / 0 skipped였다. 설치된 한글·공백 경로에서 정밀/빠른
모델로 합성 작은 흰색 크레딧을 인식했고, 두 앱 창이 각각 15초 동안 유지됐다.
설치 파일은 `YACReader-v10.3.0.15-winx64-7z-qt6.exe`, 소스는
`685d0c89e833c35193ecc6731a6e0d47cedf454e`다.
합성 작은 크레딧/흰 글자, 선택 좌표, 후보 충돌, 출처 저장 실패 롤백을 회귀 검사에 추가했다.
외부 검색은 합성 응답으로 검증하며 실제 사용자 작품의 검색 성공률은 자동 테스트 범위가 아니다.

페이지별 언어·배치 비교 및 판권 후보 연결 설치본 `10.3.0.23`은 Windows 실행
`34040703229`에서 전체 12개 CTest, 한글 경로 설치, OCR 파일 해시 대조,
두 앱의 창 시작 검증을 통과했다. 소스는 `56f345ee8a6505d62e3d0a31beb8423514abaa19`이다.
로컬 OCR 검사는 20 passed / 0 failed / 0 skipped, 설치 후 검사는
5 passed / 0 failed / 0 skipped였다. 동일한 합성 입력의 제목/이름 문자 오류는
일본어 1개, 한국어 0개였으며, 설치 전후 동일했다. 전체 판권 전사의 정확도를 뜻하지 않는다.

사용자 PC에서는 재스캔 뒤 압축작품 한 권과 이미지 폴더 한 권으로 먼저
확인한다. OCR 후보의 작가·제목을 원본 페이지와 대조해 수정한 뒤 저장하고,
작가 필터와 읽던 위치가 유지되는지 확인한다. 서명 없는 개인 개발 빌드다.

## 영역 탐지 OCR 시험 모드

새로운 패키지에는 모델 선택에 `영역 탐지 OCR (시험 · 후보 직접 확인)`이 추가된다.
기본 정밀/빠른 Tesseract 선택은 그대로다. 새 모드를 선택하고 자동, 일본어 또는
한국어로 앞·뒤 페이지를 읽을 수 있다. 폴더와 압축작품 모두 같은 경로를 사용한다.
글자 영역을 먼저 찾아 해당 영역을 일본어·한국어 전용 모델로 읽는다.
자동 언어에서는 영역마다 두 인식 모델을 비교하지만 점수는 정답 확률이 아니다.
영어 단독/세 언어 혼합 수동 선택은 기존 OCR 모드를 사용한다.

시험 모드는 후보를 제목·작가 칸에 자동 입력하거나 외부 검색을 자동 실행하지 않는다.
후보를 두 번 클릭해 확인한 뒤 기존 `메타데이터 대조…` 또는 저장을 사용할 수 있다.
그림과 글자가 섞인 실제 작품의 인식률을 확인하기 위한 선택지이며, 합성 시험 성적이
실제 만화에서의 정확도를 보장하지 않는다. 자세한 비교는 `ocr-engine-comparison.md` 참고.
모델은 설치본에 포함되고 이미지 업로드는 없다. 첫 실행은 모델 준비 때문에 더 오래 걸린다.

영역 탐지 시험 설치본 `10.3.0.26`은 Windows 실행 `34135749563`에서 빌드,
전체 12개 CTest 묶음, 모델 준비 후 실제 새 OCR, 한글 경로 설치 검증을 통과했다.
설치 후 OCR 검사 7 passed / 0 failed / 0 skipped이며 두 앱의 창 시작도 확인했다.
소스는 `03477da5194dcfb4b9bf6027d35c87b6b7fb8401`이다. 새 워커의 설치 후 실제
문자 검사는 한국어 합성 판권을 사용했으며, 일본어 인식과 탐지 엔진 비교는 별도
비교 워크플로에서 확인했다. 사용자 작품의 정확도와 혼합 언어 선택은 별도 확인 대상이다.
# Candidate consolidation and review handoff (2026-09-08)

- First-page neural text boxes can propose a title without a title label when a
  readable author credit is present and one to three larger text regions form a
  compact, consistent layout. Vertical columns use right-to-left order;
  horizontal lines use top-to-bottom order. Low-confidence pieces, sentence
  punctuation, mixed orientations and distant regions veto this inference.
- This is a review candidate, not an asserted identity. It cannot independently
  trigger an automatic lookup. It does not cover every decorative title, title
  split across many small boxes, or pages lacking an author credit.
- Equal field/value candidates share one UI row with all supporting page
  numbers. Distinct titles/authors remain separate. Each page's label and OCR
  confidence are retained in local evidence JSON; confidence is not increased
  merely because a crop rereads the same page.
- Library roots, common storage names, numeric/language/storage placeholders,
  and language-only bracket labels are excluded from name hints. A plausible
  author directory below the library root remains a low-confidence hint.
- Clicking **메타데이터 대조…** with empty inputs transfers unambiguous OCR
  candidates to the lookup review form, including source page summaries. The
  user clicks Search there before any request is made for newly proposed values.
  Conflicting candidates require a selection. Explicitly entered/selected values
  retain the existing immediate-lookup behavior.
- This change leaves bulk processing, automatic database writes, artist alias
  resolution and additional providers for later steps. Catalog Apply continues
  to preserve existing values by default and records the reviewed catalog source.
- New regression cases cover the reported split Japanese title, a slanted
  author credit, Korean title spaces, merged page evidence, root-folder hints,
  conflicting identities and review-only handoff. Packaged neural checks now
  include Japanese, Korean, and automatic language choice on both synthetic
  colophons. Windows run results are recorded in `manga-library-progress.md`.
