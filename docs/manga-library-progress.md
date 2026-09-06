# 개인용 만화 라이브러리 개발 현황

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
