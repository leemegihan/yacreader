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
