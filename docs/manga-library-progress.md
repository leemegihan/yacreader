# 개인용 만화 라이브러리 개발 현황

2026-09-09 로컬 이관 후 실제 NVIDIA 실행과 CPU 비교를 확인했다.
기본 버전 문자열은 기존과 같은 10.3.0.30이므로 빌드 소스 커밋으로 구분한다.
원본·운영 DB·기존 설치 앱을 보존하고 검증용 런타임은 별도 폴더에서 사용했다.
아래 초기 제약과 실패 기록은 작업 이력이며, 최신 검증 결과는 문서 마지막 절에 정리한다.

2026-09-06 기준. 기존 작업은 `leemegihan/yacreader` 저장소의
`manga-library-v01` 브랜치와 초안 PR #1에서 이어간다.
이번 작업 시작 커밋은 `b6556247e1da80d40a61ccb82ff4720a5cefdb58`이다.

## 목표와 유지할 구조

- Windows PC에 있는 만화 원본의 위치를 유지하고 경로와 메타데이터를 관리한다.
- 기존 YACReader의 C++/Qt 앱, SQLite DB, 압축 해제, 썸네일, Grid/List,
  읽기 진행률 및 읽기 목록 기능을 활용한다.
- 탐색은 작가·태그·Favorite/Reading/Recent 중심으로 발전시킨다.
- 수천~수만 권을 목표로 하되, 현재 성능을 검증한 것으로 간주하지 않는다.
- 외부 정보 수집보다 기본 라이브러리의 안정성과 사용성을 우선한다.

## 이전 브랜치에서 확인한 구현

| 영역 | 현재 코드 |
| --- | --- |
| 작가·태그·미확인 작품 목록 | `custom_widgets/yacreader_metadata_browser.*` |
| AniList 후보 검색과 확인 후 저장 | `custom_widgets/yacreader_metadata_lookup_dialog.*` |
| 확인 대화상자를 거치는 파일명 변경 | `custom_widgets/yacreader_filename_normalizer.*` |
| 압축 만화 앞·뒤 최대 3페이지 미리보기 | `custom_widgets/yacreader_archive_inspector_dialog.*` |
| 기존 검색 문법과 SQLite 쿼리 | `YACReaderLibrary/db/` |
| 라이브러리 로딩·갱신 및 UI 연결 | `YACReaderLibrary/library_window.cpp` |

## 이번 수정

- `QListWidget::itemDoubleClicked`와 맞지 않던 슬롯 인자 수 수정.
- 사이드바가 검색창을 찾아 조작하던 연결을 기존 `applySearchQuery()`로 연결.
  기존 macOS/Windows/Linux 검색 입력 분기를 재사용한다.
- 라이브러리 로딩 완료, 재로딩, 메타데이터 편집, XML 스캔 후 목록 갱신.
- 라이브러리를 바꾸거나 제거할 때 이전 목록과 열린 검색 대화상자 정리.
- 작품 변경 및 대화상자 종료 시 이전 HTTP 응답의 연결을 해제하고 요청 취소.
  현재 요청이 아닌 응답도 무시한다.
- 저장 완료 신호에 실제 저장한 라이브러리 경로와 작품 ID를 함께 전달.
- 읽기 전용 라이브러리에서 메타데이터 변경과 로컬 파일 검사 진입 차단.
- 관련 Qt 회귀 테스트 9개 추가: `tests/metadata_workflow_test/`.
- 저장소 스크립트로 C++ 서식 정리. DB 스키마와 파일명 변경 정책은 유지.

## 검증과 남은 제약

- 실제 C++ 코드에서 추출한 SQL로 SQLite 검사 6개 통과: 기본 보존,
  NULL/공백 채우기, 기존 태그·연도 보존, 명시적 덮어쓰기,
  비어 있는 공급자 값으로 기존 정보 삭제 방지, 없는 작품의 수정 건수 0.
  이 검사에서 읽던 페이지와 읽음 상태의 보존도 확인했다.
- Qt 회귀 테스트는 작성했지만 실행하지 못했다.
- CMake 구성은 Qt 6.9 개발 패키지 부재로 실패했다. Qt 다운로드도 작업
  환경의 네트워크 정책에 막혔다. 전체 빌드나 Windows 실행 성공으로 보고하지 않는다.
- 확인 당시 포크 저장소의 GitHub Actions 실행 기록은 0개였다.

Qt와 의존성이 있는 환경에서는 저장소 루트에서 다음을 실행한다.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 다음 단계

1. Windows 전체 빌드와 기존 Grid/List/뷰어 및 새 사이드바 동작 검증.
2. 작가·태그 탐색의 정확성과 규모 대응: 현재는 문자열을 UI 스레드에서
   읽어 목록을 만들고 기존 부분 문자열 검색을 사용하므로 태그의 정확 일치,
   복합 조건, Unicode 대소문자 처리, 비동기 집계를 별도로 설계한다.
3. 원본 이동·이름 변경 및 중복 파일에 대한 기존 스캐너의 추적 동작 검증.
4. 위 기반이 안정된 뒤 OCR, 추가 메타데이터 공급자, 일괄 검토를 진행한다.

파일명 변경에는 충돌 확인과 DB 오류 시 복구 시도가 있지만, 프로세스 중단까지
포괄하는 작업 기록 기반 복구나 일괄 되돌리기는 아직 구현되지 않았다.

## 2026-09-06: 작가·태그 정확 검색 및 Windows 자동 검증

### 검색 변경

- 사이드바는 `author:"작가명"`, `tag:"태그명"`으로 항목 하나를 정확히 검색한다.
  `Action`과 `Live Action`, `Ann`과 `Anna`가 섞이지 않는다.
- 기존 `writer:` / `tags:` 부분 검색과 `writer==` 전체 필드 일치는 유지한다.
- 쉼표, 세미콜론, 줄바꿈, 전각 쉼표·세미콜론, 일본어 쉼표(、)을 목록
  구분자로 인식한다. 사이드바 집계와 검색이 같은 구분자 정의를 사용한다.
- Unicode 대소문자 및 항목 주변 공백을 처리한다. `%`, `_`, 정규식 기호,
  따옴표, 역슬래시는 일반 문자로 검색한다. 닫히지 않은 따옴표는 오류로 처리한다.
- `tag:Action AND tag:Fantasy`, `(tag:Fantasy OR tag:Horror) NOT author:Anna`
  같은 복합 조건을 기존 검색 파서에서 처리한다. 태그가 NULL인 작품도
  `NOT tag:Action`의 결과에 포함한다.
- 정확한 작가·태그 조건을 포함한 검색은 기존 500권 제한으로 잘리지 않는다.
- DB 스키마 및 원본 만화 파일은 변경하지 않는다. 새 검색은 Qt SQLite의
  `QSQLITE_ENABLE_REGEXP` 연결 옵션을 사용한다.
  근거: https://doc.qt.io/qt-6/sql-driver.html#enable-regexp-operator
- 표기 차이(전각/반각 문자, Unicode 조합형/완성형)나 작가 별명·번역명 통합은
  이번 범위에 포함하지 않았다. 집계의 비동기화 및 색인화도 후속 과제다.

### 검증 구성

- `metadata_search_test`: 실제 C++ 파서 → 바인딩 → Qt SQLite 쿼리를 검사한다.
  정확 일치, 복합 조건, 기존 문법, 특수문자 왕복, 빈 값, 잘못된 입력,
  610권 결과, 사이드바와 검색 구분자 일치를 포함한다.
- 로컬 보조 검사: 실제 소스의 패턴 + Qt 6.9.3 정규식 엔진 + Python SQLite로
  17개 사례 통과. 2만 건 검색에서 20,002건 반환, 약 0.053초.
  이는 메모리 내 검색 조건 검사이며 GUI/실제 디스크 라이브러리 벤치마크가 아니다.
- 실제 C++ lexer를 ASan/UBSan으로 컴파일해 따옴표·경로·끝 역슬래시·닫히지 않은
  따옴표 검사를 통과했다. 이 환경의 ptrace 제약 때문에 LeakSanitizer는 끄고 실행했다.
- 로컬 전체 CMake 구성은 Qt 개발 패키지 부재로 실행 불가. 전체 빌드 및
  Qt 테스트의 판정은 GitHub Actions Windows 실행 결과를 기준으로 한다.

### Windows 작업

`.github/workflows/manga-windows.yml`:

1. Windows 2022, MSVC 2022, Qt 6.9.3에서 세 앱 및 테스트 전체 빌드.
2. 전체 CTest 실행. 실패하면 설치 파일 제공 단계로 넘어가지 않는다.
3. 기존 Inno Setup 스크립트로 x64 설치 파일 생성.
4. 필수 DLL/플러그인 검사 및 격리한 설정 폴더에서 뷰어·라이브러리 GUI 시작 검사.
5. 성공한 설치 파일, 소스 커밋, SHA-256을 30일 보관하는 Actions artifact로 저장.

검색 변경의 첫 실행 `34021303199`는 기존 파일명 변경 대화상자의
`QPushButton` 헤더 누락으로 컴파일에 실패했다. 수정은 `a4f352eb`에 반영했다.

재실행 `34021516940`는 새 테스트에서 deprecated API를 사용한 문제로 실패했다.
`d926be2a`에서 Qt 6.3 이후 지원되는 `QVERIFY_THROWS_EXCEPTION`으로 교체했고,
Windows 작업은 검색 테스트를 먼저 빌드·실행하도록 변경했다.

`34023206497`에서 Windows의 `metadata_search_test`가 통과했다. Linux의 두
압축 백엔드 빌드에서는 전체 11개 CTest가 통과했다. Windows 전체 링크는
`metadata_workflow_test`가 custom_widgets_library의 공용 MOC를 통해 불필요한
ThemeManager/사이드바 심볼까지 참조해 실패했다. 메타데이터 위젯을 별도
`metadata_widgets` 정적 라이브러리로 분리해 앱과 테스트가 동일 모듈을 재사용하도록 수정했다.

설치 스크립트의 고정 DLL 목록은 windeployqt가 배포한 루트 DLL 전체를 포함하도록
변경했다. 최종 Windows 검증은 실제 설치 파일을 임시 경로에 설치하고,
설치된 DLL들의 해시를 비교한 뒤 설치 경로에서 GUI를 시작하도록 강화했다.

### 최종 Windows 검증 성공

- 실행: https://github.com/leemegihan/yacreader/actions/runs/34023935268
- 빌드 소스 커밋: `0769195a60d6492587f1b19cdecf263e33929dd4`
- Windows x64 전체 빌드 성공, CTest 11개 묶음 전부 통과(64.33초).
- 메타데이터 화면 테스트 9개와 검색 테스트 17개도 포함한다.
- Inno Setup 설치 파일 생성 성공. 별도 임시 경로에 무인 설치 성공.
- 설치된 루트 DLL을 배포 원본과 해시 대조했고, 설치 경로에서
  YACReader와 YACReaderLibrary가 창을 생성해 각각 15초 동안 유지됨을 확인했다.
- 설치 파일 artifact: `9986595812` (약 67.7 MB ZIP).
- 실제 사용자 만화 폴더에서의 탐색, Grid/List, 읽던 위치 복원은 사용자 확인이 남는다.
- 후속 문서/번역 추출 대상 등록은 실행 코드 및 이 설치 파일을 변경하지 않는다.
  `metadata_widgets`는 `update_translations`의 SOURCE_TARGETS에도 등록했다.

## 2026-09-06: 이미지 폴더 및 로컬 페이지 OCR

- 이미지 파일이 직접 들어 있는 단일 작품 폴더를 라이브러리에 등록하고,
  압축작품과 같은 앞·뒤 페이지 확인 화면에서 처리한다. 하위 작품이 섞인
  모음 폴더는 한 권으로 합치지 않는다. 원본 이동·복사·자동 이름 변경은 없다.
- 앞·뒤 각 1~6장을 자연 정렬로 읽고, 로컬 Tesseract로 글자를 추출한다.
  일본어 가로/세로, 한국어, 영어를 지원하는 배포 구성을 추가했다.
- 페이지 글자와 근거 페이지, 낮은 신뢰의 파일/폴더 이름 힌트를 구분해 제시한다.
  사용자가 확인한 제목·작가만 저장하며 기본값은 기존 정보 보존이다.
- 판독은 OCR와 규칙 기반 후보 추출이다. 생성형 이미지 분석이나 작가 자동
  확정으로 간주하지 않는다. 이미지는 외부로 전송하지 않는다.
- 이미지 폴더의 뷰어 연결과 읽던 위치 복원을 추가했고, 재스캔/페이지 추가 시
  메타데이터를 보존하도록 했다. DB 저장은 기존 유지보수 잠금을 공유한다.
- 새 `local_metadata_test`가 폴더/압축 순서, 실제 OCR, 취소/시간제한,
  후보 역할 분리, 안전한 저장, 폴더 스캔/재스캔을 검사한다.
- Windows 검증은 OCR 엔진 소스 빌드, 언어 데이터 동봉, 전체 회귀 테스트,
  한글·공백 설치 경로에서 OCR와 GUI 실행까지 확장했다.

사용 방법과 제한: [로컬 메타데이터 OCR](local-metadata-ocr.md).

### OCR 검증 중 발견한 문제

- Windows 테스트 연결 단계에서 DB 모듈을 직접 참조하면서 생긴 의존성 문제는
  유지보수 잠금 구현을 공통 모듈로 분리해 해결했다.
- Linux/macOS의 기본 C 로캘에서는 Qt의 숫자 정렬 옵션이 적용되지 않았다.
  C 로캘일 때 숫자 정렬을 지원하는 영어 로캘로 대체해 `2.png`보다
  `10.png`가 먼저 오는 문제를 수정했다. 다른 로캘은 유지한다.
- Windows offscreen 테스트는 시스템 글꼴을 자동으로 찾지 못해 빈 합성
  표지를 만들었다. 시스템 글꼴 경로를 설정하고, OCR 전에 이미지에 충분한
  검은 글자 픽셀이 있는지도 검사하도록 보강했다.
- Windows 한글 설치 경로는 엔진의 좁은 문자열 경로 처리에 영향을 받을 수
  있어, 필요한 언어 모델을 임시 작업 폴더로 복사해 상대 경로로 읽는다.
  설치 후 앱의 실제 OCR 호출 함수로 이 경로도 검증한다.
- 별도 Linux 진단 실행 `34028634020`에서 실제 Tesseract를 사용한 새
  OCR 회귀 테스트가 통과했다. 이는 Windows 설치 검증과 별도 결과다.
- Linux 두 압축 백엔드의 전체 12개 테스트 묶음도 실행 `34028579769`에서
  통과했다. macOS 테스트에는 실행 파일 옆의 `utils/7z.so` 배포가 빠져 있어
  보완했고, 진단 실행 `34029631629`에서 macOS 및 실제 OCR을 포함한 Linux
  검사를 다시 통과했다. 이 macOS 전용 테스트 배포 변경은 Windows 앱의
  실행 코드를 바꾸지 않는다.

### OCR 포함 Windows 설치본 검증 성공

- 실행: https://github.com/leemegihan/yacreader/actions/runs/34029267025
- Windows 빌드 소스: `d85ec8e139096944143ab8e1bd91e5ca7355bc73`.
- 앱 전체 빌드, 12개 CTest 묶음 모두 성공. 로컬 메타데이터 테스트는
  실제 OCR까지 포함해 실패/건너뛰기 없이 통과했다.
- Inno Setup 설치본 생성 및 한글·공백 경로에 설치 성공.
- 배포 DLL 해시 대조, 설치된 엔진의 합성 이미지 글자 인식, 언어 데이터
  확인, 앱의 실제 OCR 호출 함수로 한글 설치 경로에서 인식 성공.
- YACReader와 YACReaderLibrary가 설치 경로에서 창을 생성하고 각 15초간
  유지됨을 확인했다. 테스트는 별도의 설정/임시 경로를 사용했다.
- 설치 파일: `YACReader-v10.3.0.13-winx64-7z-qt6.exe`.
- Artifact: https://github.com/leemegihan/yacreader/actions/runs/34029267025/artifacts/9988593850
- ZIP SHA-256: `a4621cfaf4673f000d07a76567a3ddde958e2ddea3b1686314a5012dc30a6951`.
- 설치 파일 SHA-256: `3445ea8097ec568a609ebacf195c46709715759703cb6dca327429c267a85731`.
- 내려받은 ZIP의 CRC, BUILD 소스 커밋과 두 체크섬을 직접 대조했다.
- 이후 macOS 전용 테스트 런타임 배포와 문서 변경은 이 Windows 실행
  파일의 소스를 바꾸지 않는다.

남은 사용자 확인: DB 백업 후 설치/라이브러리 업데이트를 하고, 실제
압축작품 한 권과 이미지 폴더 한 권에서 OCR 후보를 페이지와 대조한다.
작가 필터, Grid/List, 읽던 위치도 실제 라이브러리에서 확인이 필요하다.
이 검증은 실제 만화 전체에 대한 OCR 인식률을 보장하지 않는다.

## 2026-09-06: 정밀 OCR와 외부 카탈로그 대조

사용자가 앞·뒤 각 3장 처리는 확인했지만 OCR 정확도가 낮다고 보고해 다음을 추가했다.

- tessdata_best 4.1.0을 기본으로 제공하고 기존 빠른 모델도 함께 배포한다.
- 페이지 긴 변 상한을 4,000픽셀로 늘리고 작은 글자 영역을 드래그해 재인식한다.
  언어, 글자 블록/한 줄/세로쓰기, 회전, 흰 글자 반전, 배경 보정을 선택한다.
- 단일하고 명확한 역할 라벨 후보만 빈 검토 칸에 미리 채운다. 충돌 후보,
  번역자·출판사·서클, 이름 힌트는 자동 확정하지 않는다.
- OCR 제목·작가와 페이지 수를 외부 대조 화면에 전달한다. E-Hentai의 공개
  검색 결과에서 최대 12개 식별자를 얻고 문서화된 gdata API로 메타데이터만
  대조한다. 작품 링크 직접 조회와 기존 AniList 제목 검색도 지원한다.
- artist/group 및 태그 네임스페이스를 보존하고, 제목 안의 대괄호를 유지한다.
  작가 표기와 페이지 수를 함께 비교하며 제목 유사도를 정답 확률로 표시하지 않는다.
- 조회한 제목·작가·태그는 검토 후 Apply로 저장한다. 기존 정보는 기본 보존하고
  메타데이터 및 출처 기록은 같은 트랜잭션으로 저장한다. 원본 이미지는 전송하지 않는다.

검증된 소스: `685d0c89e833c35193ecc6731a6e0d47cedf454e`.

- Windows OCR 설치 검증: https://github.com/leemegihan/yacreader/actions/runs/34033636584
- 전체 12개 CTest 묶음 성공, local_metadata_test 15 passed / 0 failed / 0 skipped.
- 한글·공백 설치 경로에서 실제 정밀/빠른 모델의 작은 흰색 크레딧 인식 성공.
- 설치된 DLL 해시 확인, YACReader와 YACReaderLibrary 창 각각 15초 유지 성공.
- 일반 빌드 실행 34033639635에서도 Linux 두 압축 백엔드, macOS Universal,
  Windows x64/ARM64 및 코드 서식 검사가 모두 성공했다.
- 설치 파일: `YACReader-v10.3.0.15-winx64-7z-qt6.exe`.
- Artifact: https://github.com/leemegihan/yacreader/actions/runs/34033636584/artifacts/9989682441
- ZIP SHA-256: `a4e84141d4b64bd9b0b28cb12681f6b02fb4d3568889dda0d92b4ac67d0a8c02`.
- 설치 파일 SHA-256: `49dd47bd5d0bf5542dfe7022cfebec6a91f85694fe22fde09e62520963e58c58`.
- 다운로드한 ZIP CRC, 소스 커밋 및 설치 파일 체크섬을 직접 검증했다.
- 후속 검증 기록 커밋은 문서만 변경하며 위 Windows 실행 파일의 소스는 그대로다.

남은 범위: 실제 작품에 대한 OCR 정확도와 실제 서비스 접속/검색 성공률은
합성 테스트로 확인할 수 없다. 사용자는 이전에 오인식했던 같은 크레딧 영역을
새 정밀 모드로 비교하고, 대조 결과의 제목·작가·태그를 확인한 뒤 적용한다.
Hitomi, ExHentai 로그인 세션, 작가 별명 통합 및 검토 없는 일괄 저장은 아직 미구현이다.

## 2026-09-06: 페이지별 언어 비교와 판권 후보 연결

사용자 확인으로 초기 한국어 오인식에는 일본어+영어 설정도 영향을 줬음을 확인했다.
일본어 판권은 상당 부분 읽혔지만 `タ イ ト ル`, `発 行 者`처럼 공백이 끼어
제목 후보로 연결되지 않았다. 이번 변경은 읽힌 크레딧을 활용하는 데 초점을 둔다.

- Tesseract TSV 줄/단어 신뢰도를 읽고 페이지마다 일본어+영어와 한국어+영어를 비교한다.
  판권 라벨이 있거나 신뢰도가 낮으면 글자 블록 방식도 비교한다. 수동 언어/배치는 유지한다.
  신뢰도와 비교 점수는 정답 확률이 아니며, 언어 판정이 불확실하면 자동 조회하지 않는다.
- 공백이 끼어든 라벨 및 그 다음 줄을 연결한다. 일본어 문자 사이의 OCR 공백은 정리하고
  한국어/영어 단어 공백은 보존한다. 본문 대사를 자동 제목 후보로 넣던 규칙은 제거했다.
- 발행자·서클은 별도 후보로 남기고 작가 칸에 넣지 않는다. 번역자·인쇄소·연락처·감사
  문구는 개인 작가에서 제외한다. 뒤쪽 페이지에 제목과 작가/발행자 라벨이 모두 있으면
  날짜 라벨이 빠져도 판권 후보로 취급한다. 이는 라벨 기반 추정이며 그림 분류가 아니다.
- 파일명 괄호의 판본 표시 뒤에 있는 `[name]`도 이름 힌트로 추출한다. 일본어 이름과
  로마자 필명이 동일인이라고 추측해 저장하지 않는다.
- 명확한 단일 제목 후보는 외부 대조 화면으로 연결할 수 있다. OCR 화면의 자동 조회
  체크를 끌 수 있다. 제목 조회가 명시적으로 결과 없음이면 이름 힌트를 최대 두 번,
  5초 간격으로 조회한다. 접근 차단 화면은 빈 검색으로 간주해 반복하지 않는다.
- 제목·작가 힌트가 함께 맞고 비슷한 다른 후보나 권수/페이지 수 충돌이 없을 때만
  검토 입력칸을 미리 채운다. 기존 작가와 충돌하면 파일명 힌트로 덮어 판단하지 않는다.
  DB 저장에는 계속 Apply가 필요하며, 원본과 읽던 위치를 보존한다.

### 실제 OCR에서 확인한 제약과 검사 보완

- 실행 `34036738471`은 Windows 전체 컴파일과 메타데이터 검사 3개 묶음에 통과했으나,
  새 일본어 판권의 완전 일치 검사에서 중단했다. 설치본을 생성하지 않았다.
- 고정 Noto CJK 글꼴과 UTF-8 진단 파일을 추가한 실행 `34037675803`에서 일본어
  `青` → `育` 한 글자 오인식을 직접 확인했다. 글꼴 고정만으로 OCR 오류가 사라지지 않는다.
- 실행 `34038042946`에서는 한국어 흩어진 글자 판독의 음절 분할 오류를 확인했다.
  글자 블록 비교를 추가한 `34038377702`의 합성 한국어 판권은 제목과 작가를 정확히
  읽었지만 발행일 라벨을 누락했다. 날짜 누락 때문에 크레딧까지 버리던 분류를 보완했다.
- 실제 OCR 검사는 올바른 언어 선택, 제목과 작가/발행자 후보 연결, 핵심 식별 필드의
  한 글자 이내 오류를 검증한다. 전체 판권의 완전 전사나 실제 작품의 인식률 보장은 아니다.
  입력 이미지와 전체 UTF-8 원문을 진단 artifact로 보관해 누락도 확인할 수 있다.
- 실제 OCR 검사를 전체 앱 컴파일보다 먼저 실행한다. 한글·공백 경로에 설치한 뒤에도
  일본어/한국어 판권 검사를 필수로 실행하며 건너뛰기로 통과하지 않는다.
- 사용자의 이미지·제목·연락처는 회귀 테스트나 저장소에 넣지 않았다. 테스트는 합성 자료다.

- 실행 `34038741628`은 전체 12개 CTest, 실제 OCR 20 passed / 0 failed / 0 skipped,
  설치 파일 생성까지 통과했다. 설치된 한글 경로에서는 일본어 모델이 내부에서 읽는
  `jpn_vert`가 임시 모델 폴더에 없어 실패했다. 검증 설치본으로 배포하지 않았다.
- `b6e0b67e`에서 일본어의 세로쓰기 하위 모델도 임시 복사하도록 수정했다.
  전체 앱 빌드 전 실제 OCR 검사부터 한글 모델 경로를 사용해 이 조건을 검사한다.

- 실행 `34039671164`에서는 하위 모델 누락이 해결됐고 전체 테스트와 설치 파일 생성이
  통과했다. 설치 후 검사에서는 일본어 식별 필드의 문자 오류가 4개여서 중단했다.
  이전 검사는 offscreen과 일반 Windows Qt 환경에서 각각 이미지를 새로 그렸다.
- 설치 검증은 같은 PNG 입력을 재사용하도록 바꿨으며, 일반 Windows Qt에서도 전체
  빌드 전에 이 입력으로 검사한다. 핵심 식별 필드의 한 글자 이내 기준은 유지한다.
  설치된 모든 OCR 엔진·모델·의존 파일의 해시도 배포 원본과 대조한다.

### 최종 Windows 설치본 검증 성공

- 실행: https://github.com/leemegihan/yacreader/actions/runs/34040703229
- 앱 소스: `56f345ee8a6505d62e3d0a31beb8423514abaa19`. 이후 검증 기록 커밋은 문서만 변경한다.
- 전체 12개 CTest 묶음 통과. 실제 엔진을 포함한 로컬 OCR 검사는
  20 passed / 0 failed / 0 skipped였다.
- 일반 Windows Qt 환경에서도 같은 PNG 입력으로 일본어·한국어 OCR 검사가 통과했다.
- 한글·공백 경로에 설치한 뒤 실제 OCR 검사는 5 passed / 0 failed / 0 skipped.
  합성 제목/이름의 문자 오류는 일본어 1개, 한국어 0개로 설치 전후 동일했다.
  이는 이 두 합성 자료의 결과이며 실제 작품의 정확도나 전체 판권 전사율이 아니다.
- 설치된 OCR 엔진·언어 모델·의존 파일 및 앱 DLL의 해시를 배포 원본과 대조했다.
  YACReader와 YACReaderLibrary가 각각 창을 만들고 15초 동안 실행 상태를 유지했다.
- 일반 빌드 실행 34040706334에서도 Linux 두 압축 백엔드, macOS Universal,
  Windows x64/ARM64 및 코드 서식 검사가 성공했다.
- 설치 파일: `YACReader-v10.3.0.23-winx64-7z-qt6.exe`.
- Artifact: https://github.com/leemegihan/yacreader/actions/runs/34040703229/artifacts/9991816941
- 설치 파일 SHA-256: `04cba807db1cafbc234e465de06ad8b973761a1efcdf8d4067c64d9547f773ff`.
- ZIP SHA-256: `1b0132208b2ea36a83b6b11046340e69d231bab2af17ebfd34fb974c0e46c663`.
- 내려받은 ZIP CRC, 소스 커밋 및 설치 파일 체크섬도 직접 확인했다.

사용자 확인: 기존 두 앱을 종료하고 새 설치본을 설치한 뒤, 같은 작품을 기본 자동 모드로
읽어 제목·작가 후보와 외부 대조 결과를 확인한다. 실제 서비스 접근과 실제 작품의
OCR/검색 성공률은 사용자 PC에서 확인해야 한다. DB 적용 전 후보 검토는 계속 필요하다.

## 2026-09-07: 영역 탐지 OCR 비교 및 Windows 시험 설치본

- `5fec72dc`와 `eb8f9bea`: Windows CPU에서 동일한 합성 8장과 빈 페이지를
  Tesseract / 탐지+Tesseract / 탐지+PaddleOCR / 일본어 탐지+Manga OCR로 비교했다.
  제목·작가 필드 복구는 각각 11/16, 12/16, 16/16, 일본어만 8/8이었다.
  Mobile 탐지로 PaddleOCR 처리 시간을 약 15~18초에서 1.65~3.25초/장으로 줄였다.
  초기 모델 준비/프로세스 실행 시간은 제외한 값이며, 실제 만화 인식률은 아니다.
- `03477da5194dcfb4b9bf6027d35c87b6b7fb8401`: mobile 탐지와 일본어/한국어
  PaddleOCR를 선택 가능한 `영역 탐지 OCR (시험 · 후보 직접 확인)`으로 연결했다.
  폴더·압축 공통 페이지 처리, 취소, 원본/DB 보존을 유지한다. 기본값은 기존 정밀 OCR이다.
  새 워커는 별도 CPython으로 실행하고 로컬 모델만 사용한다. 모델 9개 파일을 해시 고정했다.
- 시험 결과는 자동 입력·자동 외부 검색에서 제외한다. 사용자가 후보를 선택한 뒤 기존
  외부 메타데이터 대조와 저장 기능을 사용할 수 있다. 먼 위치의 글자를 라벨에 잘못
  연결하지 않도록 위치 검사를 추가했다. 재판독한 crop과 원래 페이지의 좌표는 혼합하지 않는다.
- Windows 실행 `34135749563` 성공. 전체 12개 CTest 묶음 통과.
  모델 준비 전 로컬 OCR 검사 21 passed / 0 failed / 1 skipped이며, 새 실제 OCR 검사는
  모델 준비 후 별도로 4 passed / 0 failed / 0 skipped로 실행했다.
  한글 경로 설치 후 기존/새 OCR 및 응답·검토 검사는 7 passed / 0 failed / 0 skipped였다.
  설치 파일 전체 OCR 의존 바이트 대조, YACReader/YACReaderLibrary 창 시작도 통과했다.
- 설치본: `YACReader-v10.3.0.26-winx64-7z-qt6.exe`.
  패키지에는 Python과 모델이 포함돼 별도 설치/API 키가 필요 없다.
  실제 작품의 표지, 세로 글자, 복잡한 판권, 자동 언어 선택의 정확도는 사용자 확인이 남아 있다.
  설치 후 새 시험 모드를 명시적으로 선택해 현재 시험 작품으로 비교한다.
- 재현 자료: `docs/ocr-engine-comparison.md`, `docs/ocr-comparison-mobile.json`,
  `tools/neural_ocr/models.lock.json`. 최종 패키지의 전이 의존 버전도 constraints로 고정한다.
- 후속 방향: 실제 작품에서 탐지 누락/문자 오류/역할 연결 오류를 구분하고,
  모델을 페이지마다 다시 읽는 비용과 세로쓰기/기울기/복잡한 읽기 순서를 개선한다.
  합성 결과를 근거로 일괄 자동 확정이나 엔진 전면 교체를 하지 않는다.

## 2026-09-08: OCR 후보 통합과 외부 대조 연결

- 영역 탐지 OCR이 표지 제목을 여러 줄/세로 열로 나눈 경우, 큰 글자 영역의
  배치와 작가 크레딧을 이용해 제목 후보를 조합한다. 세로 열은 오른쪽부터,
  가로 줄은 위에서부터 연결한다. 기울어진 크레딧의 큰 바운딩 박스도 고려한다.
  저신뢰 글자, 문장 부호, 떨어진 영역, 방향 충돌은 조합을 차단한다.
- 조합한 표지 제목은 추정으로 표시한다. 모든 장식 제목을 처리하는 것은 아니며,
  이 추정만으로 자동 검색하거나 DB에 저장하지 않는다.
- 같은 제목/작가가 앞·뒤 페이지에서 나오면 하나의 후보로 묶어 근거 페이지를
  함께 표시한다. 서로 다른 후보는 유지한다. 페이지별 라벨/신뢰도는 로컬
  근거 JSON에도 남기며, 같은 페이지 재판독을 추가 페이지 증거로 세지 않는다.
- 라이브러리 최상위 폴더, `TestLibrary`, 언어/일련번호/저장형태 조합 같은
  정리용 이름을 작가 힌트에서 제외한다. 의미 있는 작가 하위 폴더는 낮은 신뢰의
  힌트로 유지한다. Windows의 경로 표기 차이도 디렉터리 비교로 처리한다.
- 입력칸이 비어 있을 때 `메타데이터 대조…`를 누르면 유일한 OCR 후보를 제목·작가
  및 근거 페이지와 함께 검토 화면으로 넘긴다. 이 경우 검색 버튼을 눌러야
  외부 요청이 발생한다. 충돌 후보가 있으면 선택을 요구하고, 이미 직접 선택한
  값은 기존 즉시 조회 동작을 유지한다. Apply 전에는 DB를 변경하지 않는다.
- 회귀 검사에 표지 제목 조합/거부 조건, 한글 띄어쓰기, 중복 근거, 최상위 경로,
  충돌 선택, 검색 전 검토, 작품 전환 시 근거 초기화를 추가했다.
  설치 전·후 실제 영역 탐지 OCR 검사는 한국어/일본어와 자동 선택 두 경우를
  합쳐 네 가지로 확장했다.
- 첫 Windows 실행 `34186870845`에서 루트 경로 비교 검사가 실패해 설치본을
  만들지 않았다. `df6b274e4d749f26d6656841831d3309104ffaa5`에서 수정했다.

### Windows 검증 결과와 설치본

- 검증 실행: https://github.com/leemegihan/yacreader/actions/runs/34187266390
- 앱 소스: `df6b274e4d749f26d6656841831d3309104ffaa5`. 이후 기록 커밋은 문서만 변경한다.
- 전체 앱 빌드 및 12개 CTest 묶음 통과. 모델 준비 전 로컬 OCR 검사는
  26 passed / 0 failed / 4 skipped였다. 건너뛴 네 언어 설정은 모델 준비 후
  필수 검사로 별도 실행해, 응답 검사와 함께 7 passed / 0 failed / 0 skipped를 기록했다.
- 한글·공백 경로 설치 후 기존/영역 OCR 및 후보 검사는
  15 passed / 0 failed / 0 skipped. 한국어, 일본어, 자동-한국어, 자동-일본어
  모두 합성 판권의 기대 제목·이름 문자열을 읽었다. 실제 만화의 인식률을 뜻하지 않는다.
- 설치된 OCR 파일 전체와 Qt DLL 해시가 배포 원본과 일치했다.
  개발용 Qt 경로 없이 두 앱이 각각 창을 만들고 15초 동안 실행 상태를 유지했다.
- 일반 빌드 `34187269259`의 Windows x64/ARM64, Linux 두 압축 백엔드,
  Docker 및 서식 검사도 통과했다. macOS 앱과 테스트 바이너리는 컴파일됐으나
  DMG의 `hdiutil detach` 단계가 `No such file or directory`로 실패해
  macOS 테스트는 실행되지 않았다. 이번에 macOS 설치본을 검증한 것으로 보지 않는다.
- 설치 파일: `YACReader-v10.3.0.28-winx64-7z-qt6.exe`.
- Artifact: https://github.com/leemegihan/yacreader/actions/runs/34187266390/artifacts/10041426516
- 설치 파일 SHA-256: `52d1998b12df68fa4133f1da8a62647a4f53ccad0f6812a91b3c78dbcac7a929`.
- ZIP SHA-256: `0fa3f27f542650a7a2becf36fe2286e9fdc46dcc6f197fe4984c0ab9abd7e032`.
- 내려받은 426,814,004바이트 ZIP의 CRC, GitHub artifact 해시, 포함된 소스 커밋,
  설치 파일 체크섬을 직접 대조했다.

사용자 확인: 두 앱을 종료하고 설치한 뒤 기존 시험 작품을 영역 탐지 OCR로 읽는다.
같은 제목·작가가 여러 페이지에서 나온 후보의 근거 페이지가 함께 표시되는지,
입력칸이 비어 있어도 `메타데이터 대조…`에서 유일한 후보와 근거가 전달되는지 확인한다.
새로 제안된 값은 대조 창에서 검색 버튼을 눌러 조회한다. 생성한 가상 시험 작품은
외부 사이트의 실제 등록 작품이 아니므로 검색 결과가 없어도 정상이다.

### 전체 라이브러리 일괄 처리 방향

이번 변경은 개별 작품에서 후보와 근거를 안정적으로 연결하는 단계다. 폴더와
압축작품 공통 처리 구조를 유지하지만, 전체 라이브러리를 순회하는 작업 큐와
무인 일괄 저장은 아직 구현하지 않았다. 후속 작업은 처리 상태/중단·재개를
저장하는 큐, OCR 결과 재사용, 외부 조회 간격과 오류 재시도, 일치 근거가 충분한
항목과 확인 필요 항목의 분류, 기존 값 보존 및 변경 되돌리기를 포함해야 한다.
작가 별명 통합, Hitomi, 인증된 ExHentai 세션 연동도 아직 구현하지 않았다.

## 2026-09-08: 작가 역할 연결과 OCR 처리 속도

실제 작품 테스트에서 표기된 이름이 작가 후보에 연결되지 않거나, 본문 대사가 작가로
잡히는 사례를 확인했다. 사용자가 제공한 페이지나 실제 작품 정보는 저장소에 넣지 않고
합성 이름/문장/위치 데이터로 회귀 검사를 만들었다.

- `이름 지음`, 이름 아래 `지음`, 가까운 두 줄의 필명, `誌名`, `発行／著者`를 지원한다.
  복합 역할에서는 발행자와 작가를 구분한다. 역할만 읽고 이름을 놓쳤으면 추측하지 않는다.
  대사 형태와 멀리 떨어진 글자를 작가로 연결하는 조건을 제한한다.
- 한 작품의 최대 12장을 한 프로세스로 읽고 모델을 재사용한다. 영역 인식은 최대 8개씩
  묶으며 CPU 사용량을 최대 4/8/16스레드 중 선택할 수 있다. 실제 논리 CPU 수로 제한한다.
- 페이지 완료 수, 처리 단계, 경과 시간과 실제 사용 장치를 표시한다. 읽힌 글자의 점수가
  탐지 누락까지 평가한 값은 아니라는 설명을 화면에 명시한다.
- 사각형 글자 영역의 기울기 보정, 분리 가능한 세로 한글의 가로 재배열 비교, 작은 판권
  라벨 아래의 제한된 확대 재탐지를 추가한다. 미인식/장식 글자 전체를 복원하는 기능은 아니다.
- 새 영역 OCR의 직접 검토/출처/저장 정책과 원본 경로·읽던 위치 보존을 유지한다.

### 기본 Windows 설치본 검증

- 검증: https://github.com/leemegihan/yacreader/actions/runs/34229266510
- 앱 소스: `89c52c3467e57f18bf58e74b02ee48f0ea4a2738`.
- 첫 구현 `b24fb93b`의 초기 회귀 검사는 통과했으나, 설치 경로 검토 중 모델 경로를
  상대 경로로 유지해야 하는 부분을 보완했다. 첫 기본 빌드는 새 실행으로 대체했다.
- 전체 앱 빌드 및 12개 CTest 묶음 통과. 모델 배포 전 로컬 OCR 28 passed / 0 failed /
  5 skipped이며, 실제 새 모델 검사는 별도 필수 단계에서 실행했다.
- Python 영역 보정/묶음 인식/출력 검사 7개 통과. 실제 영역 OCR 8 passed / 0 failed /
  0 skipped. 일본어, 한국어, 각 자동 선택과 6페이지 모델 재사용을 포함한다.
- 한글·공백 경로 설치 후 OCR/후보 검사 18 passed / 0 failed / 0 skipped. 설치된
  OCR/Qt 파일의 해시를 원본과 대조했고, 개발 Qt 경로 없이 두 앱의 창 시작을 확인했다.
- 같은 엔진·설정·합성 한국어/일본어 6장의 자동 모드 비교: 페이지마다 프로세스를 실행하면
  54,095ms, 한 프로세스에서 재사용하면 34,961ms로 약 35.4% 단축됐다. 설치 후 같은
  재사용 검사는 35,842ms였다. 이전 설치본 전체와의 비교나 사용자 PC 실측값은 아니며,
  스레드 증설/GPU의 속도 향상을 측정한 결과도 아니다.
- 설치 파일: `YACReader-v10.3.0.30-winx64-7z-qt6.exe` (430,004,570 bytes).
- Artifact: https://github.com/leemegihan/yacreader/actions/runs/34229266510/artifacts/10058169211
- 설치 파일 SHA-256: `f9c2d7d8a45d3288eb2c11e3fd55b88b1dc4e7d06cb4809b91cb37fc118a031f`.
- ZIP SHA-256: `dd25ff34662e96be3840c811f0557b6e412ab34ba843e07f92bec4cd70927fbe`.
- 내려받은 426,829,146-byte ZIP의 CRC, artifact digest, 포함된 소스와 EXE 해시를 확인했다.
- 일반 빌드 `34229270648`에서도 Windows x64/ARM64, Linux 두 압축 백엔드,
  macOS Universal, Docker amd64 및 코드 서식 검사가 통과했다. Docker arm64의
  첫 시도는 컴파일 전 `ghcr.io`의 HTTP 429 요청 제한으로 실패했다. 재실행에서도
  `ports.ubuntu.com` DNS 해석 실패로 의존 파일을 받지 못했다. ARM Docker 이미지는
  이번에 검증되지 않았으며, Windows 설치본 검증과는 별도다.

### 선택 NVIDIA 구성 요소 검증

- 최종 검증: https://github.com/leemegihan/yacreader/actions/runs/34234681876
- 패키징 소스: `fe718667ca9346796af06e6308292fc46a0a0a7b`.
  기본 앱 소스 `89c52c3` 이후 변경은 GPU CI의 검증 브랜치와 설치 파일 분할 구성이다.
  앱과 OCR 워커는 검증된 기본 설치본과 동일하다.
- 공식 PaddlePaddle GPU 3.2.2 / CUDA 11.8과 고정한 OCR 의존 파일을 묶었다.
  의존성 검사와 CUDA 빌드 확인을 통과했다. Windows 실행기에 실제 GPU는 없었다.
- 첫 GPU 실행 `34228684449`에서는 한글 절대 모델 경로가 네이티브 로더에서 실패했다.
  `89c52c3`에서 모델 폴더 기준 상대 경로로 수정했다. 후속 실행 `34230736317`은
  설치 후 OCR까지 통과했으나 약 2.36GB 단일 artifact를 전달 도구로 내려받을 수 없었다.
- 최종 설치 프로그램은 Inno Setup 기본 다중 볼륨 기능으로 EXE 하나와 BIN 다섯 개를
  만든다. 각 볼륨을 별도 ZIP artifact로 올려 개별 파일을 512MiB 미만으로 유지한다.
  첫 ZIP에는 EXE, 첫 BIN, 전체 파일 체크섬 및 빌드/CPU 전환 검사 기록이 포함된다.
- 설치 EXE가 모든 볼륨을 읽어 한글·공백 경로에 설치되는지 확인했다. 설치된 런타임
  전체 해시가 배포 원본과 일치했다. 개발 PATH를 지운 뒤 설치된 Python, 워커와 모델로
  GPU를 요청했고, GPU 부재 경고와 CPU 전환 및 합성 이름의 실제 문자 인식을 확인했다.
- 실제 RTX 4070 Ti SUPER에서 GPU 추론과 CPU 대비 속도는 아직 측정하지 않았다.
  사용자 PC에서는 기본 설치본 다음에 같은 폴더로 이 구성 요소를 설치하고 앱을
  재시작한 뒤, 실제 처리 장치와 같은 작품의 후보/소요 시간을 확인해야 한다.

| 전달 ZIP | GitHub artifact ID | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| YACReader-NVIDIA-OCR-Part-1.zip | 10059879836 | 480003199 | bd21e7837c0c58f163ec936df70dc74417e6d8474cb80cb2512bab128d470e02 |
| YACReader-NVIDIA-OCR-Part-2.zip | 10059884841 | 480000182 | 5c5f82347a0cd83be5aa03f0163813fcf09024d446dd1d3fb095c57e6c4353d1 |
| YACReader-NVIDIA-OCR-Part-3.zip | 10059889346 | 480000182 | 6fa701b4e7d698a6f2044402fe4cba9a264eba25a380fef4a4313dcaca2c89ab |
| YACReader-NVIDIA-OCR-Part-4.zip | 10059898273 | 480000182 | 3fb8df703cc5194e907a0af280f23c763533e7e080471df1d6cf9eb3641799d2 |
| YACReader-NVIDIA-OCR-Part-5.zip | 10059902554 | 444192064 | d2a65cb2967e26e06997f0fad7b50d24a873502f3786f43176f2854415fd1a77 |

다섯 ZIP을 실제로 내려받아 각 크기/artifact 해시/CRC를 검증했다. EXE 하나와 BIN
다섯 개의 크기 및 SHA-256이 첫 ZIP의 전체 manifest와 일치하며, 빌드 소스와 CPU 전환
검사 결과도 대조했다. 실행 파일 `YACReader-NVIDIA-OCR-3-win64.exe`는 4,018,422 bytes,
SHA-256 `8e96f5f34760d84ce1fe6822cbc300a22e8e5960995023626834e773f58df4e0`이다.

기본 앱과 NVIDIA 구성 요소의 사용법은 `local-metadata-ocr.md`에 기록했다. 모든 ZIP을
같은 폴더에 푼 뒤 EXE를 실행하며, 기본 앱과 동일한 설치 폴더를 선택한다.

## 2026-09-09 로컬 이관 및 OCR 검증

기록 커밋 f8bdf1c에서 기존 개발 브랜치를 이어갔다. 애플리케이션 변경은 fcc85308,
유니코드 경로 테스트 보완은 9e18d424, 검증 아티팩트 포장 보완은 19f0b69d이다.
실제 작품·경로·OCR 문자열·후보·상세 측정값·검토 정답은 로컬 기록에만 보관한다.

### 확인한 동작과 보완

- 선택 NVIDIA 런타임으로 실제 GPU 추론을 확인했다. 워커의 반환 장치뿐 아니라
  해당 OCR 프로세스가 NVIDIA 장치 목록에 나타나는지도 대조했다.
- 동일한 고정 표본과 앱의 Qt 디코딩/전처리로 CPU 4·8·16스레드와 GPU를 비교했다.
  초기 외부 이미지 전처리 비교는 앱 전처리 실측과 구분해 보관한다.
- 원래 이미지 폴더형 작품을 찾지 못한 범위에서는 선정 표본의 파생 폴더 복사본으로
  압축/폴더의 페이지 수·순서·준비된 픽셀을 대조했다. 독립된 원본 폴더 작품 검증으로 세지 않는다.
- 공용 작가/서클 라벨은 미확정 작가·출판/서클 대안을 제시한다. 명시적 발행 서클은
  출판/서클로 연결한다. 공용 역할을 자동 작가 확정이나 자동 외부 검색에 사용하지 않는다.
- 선택적 워커 진단은 탐지 상자와 낮은 신뢰도로 배제된 문자 후보까지 기록한다.
  기본 OCR 반환 내용은 유지하며 실제 진단 자료는 공개 저장소에 올리지 않는다.
- 명시적 로컬 테스트에서 C++ 경로의 GPU 구성 요소 부재, GPU 실행 오류 후 CPU 재시도,
  실제 GPU 실행을 구분해 확인했다. 장치 테스트의 문자 정답은 합성 JP/KR 자료다.
- YACREADER_DATA_DIR로 앱/공유 설정과 로그를 격리한다. 한글 Windows 환경의 테스트는
  유니코드 CRT 환경 변수를 사용해 경로의 손실을 막고 설정/해제/복원을 검사한다.
- Windows 포맷 스크립트가 clang-format 실패를 전파하도록 수정했다.

### Windows 배포 검증

일반 설치 프로그램은 기존 버전 제거 훅을 실행할 수 있으므로 로컬 운영 PC에서
설치 경로만 바꿔 실행하지 않았다. 실제 설치는 격리된 Windows CI에서 검증하고,
로컬에서는 검증 아티팩트의 독립 실행 파일/런타임을 사용했다.

첫 검증 아티팩트에서 업로드 기본 설정이 숨김 PaddleX 버전 파일을 제외하는 문제를
발견했다. 숨김 파일을 포함하고, CI가 업로드한 아티팩트를 다시 내려받아
실제 OCR·CPU 전환 및 설정 격리 검사를 실행하도록 보완했다.

최종 코드 검증: https://github.com/leemegihan/yacreader/actions/runs/34253793171
일반 플랫폼 빌드: https://github.com/leemegihan/yacreader/actions/runs/34253797241
최종 Windows CI는 전체 CTest, 설치 후 OCR과 GUI 시작, 업로드/다운로드 후 OCR을 통과했다.
새 아티팩트를 보완 없이 별도 로컬 폴더에 풀어 한글 경로 설정 격리, CPU 전환,
실제 GPU 실행과 워커/모델 일치를 확인했다. 일반 플랫폼 빌드도 모두 통과했으며,
macOS 임시 디스크 해제 오류는 해당 작업 재시도에서 해결됐다.
합성 검사는 Python 10개, CTest 12개 묶음, 설치 후 21개 및 아티팩트 재다운로드 후 4개가 통과했다.

### 다음 범위

글자 탐지 누락, 문자 오인식, 올바르게 읽은 이름의 역할 연결 실패를 구분하는 기준을
마련했지만, 전체 실제 작품의 정답 세트는 아직 확정하지 않았다. 장치 간 문자열/후보
일치나 합성 테스트 성공을 실제 작품의 정답 정확도로 부르지 않는다.

다음 단계는 로컬 검토 목록의 제목·작가·출판/서클 정답과 난이도를 확인하고
개별 작품의 후보 확보 범위를 개선하는 것이다. 영구 일괄 큐, 중단·재개, OCR 캐시,
일괄 저장과 전체 라이브러리 실행은 이번 작업에서 진행하지 않았다.

## 2026-09-09 — explicit credit separators and inline brackets

- Filled-circle separators (●/◉) now connect complete explicit role labels to values. Publisher and printer credits remain separate from authors; ambiguous author/circle credits still require review.
- Bracketed labels require a name on the same OCR line. Standalone bracket labels do not borrow a following social-media heading as an identity.
- Added synthetic regressions for publishing roles, dialogue rejection, value punctuation, ambiguous roles, and standalone bracket labels. The worker and models are unchanged; automatic language selection can still discard a more complete alternative, so inspect local diagnostics or choose a language explicitly.
- Code `703ce1c5d0bccda77948fb402d4c9904e7f834ab`: [Windows validation](https://github.com/leemegihan/yacreader/actions/runs/34293714653) passed all 12 CTest suites, 10 Python worker tests, required staged OCR (8 passed), installed OCR/candidate checks (21 passed), GUI startup, and downloaded-artifact OCR/path checks (4 passed). Prerequisite-stage skips remain distinct from required packaged checks.
- [General platform build](https://github.com/leemegihan/yacreader/actions/runs/34293718414) passed formatting, Windows x64/ARM64, both Linux backends, macOS Universal and Docker amd64/arm64 without a retry.
- The freshly downloaded validation runtime passed 8 targeted local tests plus initialization/cleanup, with unchanged worker/model hashes. Real input evidence and fixed-result comparisons remain local. No real media or private OCR output belongs in Git.
- This entry records verification only. Full-library processing and unreviewed metadata writes remain out of scope.
## 2026-09-09 — review partial cross-language OCR without replacing primary text

- Keep the confidence winner as the primary OCR reading. Preserve bounded high-scoring cross-language alternatives when an ASCII token is contained in a longer CJK reading; this is a review heuristic, not a correctness decision, and requires no additional inference pass.
- Display both readings and offer only same-line explicit credit alternatives as unlabelled candidates. They do not borrow adjacent primary text, infer cover titles, autofill review fields or initiate automatic lookup. An explicit action opens review with unconfirmed evidence.
- Validate the optional response field and map retry-crop alternatives to the parent's page coordinates. Older worker responses remain compatible; primary text and aggregate confidence exclude alternatives.
- Code `d3e6fb02d808fc1115e86843c56f2fc182e3461b`: [Windows validation](https://github.com/leemegihan/yacreader/actions/runs/34297242620) passed all 12 CTest suites, 13 Python worker tests, required staged OCR (8 passed), installed OCR/candidate checks (21 passed), GUI startup, and downloaded-artifact OCR/path checks (4 passed). Prerequisite-stage skips remain separate from required packaged checks.
- [General build](https://github.com/leemegihan/yacreader/actions/runs/34297246604) passed Windows x64/ARM64, both Linux backends, macOS Universal, Docker amd64/arm64 and formatting.
- A fresh independent runtime passed 11 targeted local tests plus initialization/cleanup. Its worker matched the local real-input test version and all nine model files remained unchanged. Missing-addon CPU fallback and actual NVIDIA inference then passed separately through the C++ wrapper with synthetic JP/KR fixtures in that new validation directory.
- Fixed-input comparisons, real transcriptions, reviewed regions, candidates and timings remain outside Git. Original media, the operating database and the currently installed app were preserved; whole-library processing remains out of scope.
## 2026-09-09 — review joined author text in publishing rows

- Add a conservative review proposal for a short katakana pen name joined to the Japanese author label when high-scoring, aligned publishing and printing rows support the layout. Missing delimiters remain uncertain; ordinary credit parsing and automatic evidence are unchanged.
- Synthetic regressions cover missing/low-quality geometry, unrelated rows, duplicate roles, dialogue, review UI, independent explicit evidence and cover-title isolation. Worker and models are unchanged; compare saved fixed CPU/GPU outputs without repeating inference.
- Code `5589a88b04ca1ca4108a8a57644ba11b0537a64b`: [Windows validation](https://github.com/leemegihan/yacreader/actions/runs/34300994342) passed all 12 CTest suites, 13 Python worker tests, staged neural checks (8 passed), installed OCR/candidate checks (21 passed), GUI startup and downloaded-artifact checks (4 passed). The new joined-credit regression passed; prerequisite-stage skips remain distinct from required packaged checks.
- [General build](https://github.com/leemegihan/yacreader/actions/runs/34300998058) passed formatting, Windows x64/ARM64, both Linux backends, macOS Universal and Docker amd64/arm64 without a retry.
- A freshly downloaded independent runtime passed 12 targeted local tests plus initialization/cleanup. Its artifact hash and hidden PaddleX metadata were verified; worker and all nine model files match the previous validation runtime. The download service returned a transient HTTP 520 once; retry succeeded and the ZIP matched its published digest.
- Replay of saved real-input CPU/GPU outputs preserved existing candidates and kept both device candidate sets equal. A reviewed missing author became an unconfirmed proposal. This parser-only change did not repeat inference or speed measurements; earlier physical-GPU verification, current synthetic installation/CPU-fallback checks and candidate replay are separate evidence.
- Narrow-text review identified remaining cross-language selection errors; CJK disagreements need a separate worker experiment. Real text, paths, candidates, measurements and review answers remain local. No whole-library processing, operating database writes or current-app installation changes were made.

## 2026-09-09 — preserve disputed CJK readings for explicit review

- Extend alternative retention to different CJK readings of the same region when both models score at least 85 and the gap is at most 8. Preserve the primary winner and its score, deduplicate normalized text and retain at most two alternatives. This is a review-volume heuristic, not score calibration or automatic language correction.
- Existing ASCII-extension review behavior remains available. No additional inference call, model, schema version or automatic metadata action is introduced. Candidate explanations now also describe disagreement rather than only extra text.
- Add synthetic worker tests for both language directions, ties, boundaries, duplicate/unsupported readings, region equality, limits and unchanged inference-call count. A C++ inspector regression verifies disputed CJK titles remain unconfirmed and require an explicit review action.
- Code `6da32db666d0cfafbe1424d114f812146fa47e2c`: [Windows validation](https://github.com/leemegihan/yacreader/actions/runs/34307715361) passed all 12 CTest suites, 16 Python worker tests, staged neural checks (8 passed), installed OCR/candidate checks (21 passed), GUI startup and downloaded-artifact checks (4 passed). The disputed-title review regression passed. Prerequisite-stage skips remain separate from required packaged checks.
- [General build](https://github.com/leemegihan/yacreader/actions/runs/34307718102) passed formatting, Windows x64/ARM64, both Linux backends, macOS Universal and Docker amd64/arm64 without retry.
- A fresh independent runtime passed 13 targeted local tests plus initialization/cleanup. Its ZIP digest and hidden PaddleX metadata were verified. The locally exercised worker used LF and the Windows artifact used CRLF: exact hashes were recorded separately, while newline-normalized source and Python AST matched. All nine model files remained byte-identical. A second untouched extraction supplied the final local checks; the first copy was preserved after the initial strict byte comparison stopped.
- The fixed real-input GPU/CPU matrix retained each device's primary readings, existing alternatives and metadata candidates. Reviewed language-selection errors are now available as alternatives; candidate agreement and synthetic success are not whole-work accuracy measurements. Alternative counts can differ by device because the other model's reading or score differs.
- Separate physical-GPU checks observed the actual worker PID in the NVIDIA process list, including a check using the exact packaged worker bytes. A targeted CPU diagnostic explained a device-dependent alternative threshold result. These additional checks were excluded from the paired timing matrix.
- Real inputs, text/role truths, candidate outputs, paths and timings remain private. No whole-library processing or operating-app/database changes were made. Further work prioritizes unresolved identity roles and candidate-free works within the existing fixed sample.

## 2026-09-09 — preserve joined circle banners for review

- Preserve a compact, high-scoring two-part circle banner near the top of an opening page as one unconfirmed publisher/circle proposal when OCR loses the label boundary. Do not split its parts into people/circles or use it as author/title evidence. Plain text, body-page and unrelated-layout cases retain strict parsing.
- Automatic lookup now excludes publisher/circle hints without strong independent evidence. Explicit lookup carrying tentative publishing hints opens review even when the title was already filled. Publisher-only proposals do not initiate title/author search.
- Synthetic regressions cover proposal boundaries, missing layout, dialogue, role isolation, unchanged explicit labels, autofill and both automatic/manual lookup paths. Worker/models and inference scheduling are unchanged; real candidate validation replays saved fixed CPU/GPU outputs.
- Validation pending for this code revision. Real text, paths, candidates and measurements remain local; no original media, operating DB or current installation changes.
