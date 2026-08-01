#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QJsonObject>

namespace FileHelper {

// Set file creation/modification times
void setFileTimes(const QString &filePath, const QDateTime &timestamp);
void setFileTimes(const QString &filePath, const QString &timestampStr);

// Add macOS Finder comment
void setFinderComment(const QString &filePath, const QString &comment);

// Sanitize filename
QString sanitizeFilename(const QString &name, int maxLength = 200);

// ★ 산출물을 최종 저장 위치로 안전하게 옮긴다.
//   QFile::rename 만 쓰면 두 경우에 실패한다 — (1) 대상이 다른 디스크(NAS·외장)일 때,
//   (2) 같은 이름이 이미 있을 때. 실패를 확인하지 않으면 원본까지 지워 파일이 유실된다.
//   이 함수는 대상 폴더 생성 → 기존 파일 정리 → rename 시도 → 실패 시 스트림 복사로 폴백하고,
//   복사가 완전히 끝난 뒤에만 원본을 지운다(중간에 끊겨도 원본은 남는다).
//   성공 true. 실패 시 err 에 사유가 담기고 원본은 그대로 보존된다.
bool moveFileSafe(const QString &srcPath, const QString &dstPath, QString *err = nullptr);

// ★ 파일명 규칙 — 기본은 '윈도우 호환'(NTFS·exFAT·윈도우 기반 NAS·SMB 에서도 저장되게
//   : * ? " < > | \ 를 모양이 같은 전각 문자로 치환). NAS 가 순수 유닉스(ext4/Btrfs)면
//   true 로 켜서 특수문자를 원문 그대로 보존한다.
void setUnixFilenames(bool on);
bool unixFilenames();

// Ensure directory exists
bool ensureDir(const QString &path);

// macOS 다운로드 메타데이터 설정 (xattr — QProcess 없이 직접 syscall)
void setDownloadMeta(const QString &filePath, const QString &url);

// ──────────────────────────────────────────────────────────
// 공통 저장 정책 helpers (플랫폼 간 일관된 폴더/파일명 정책)
// ──────────────────────────────────────────────────────────

// 업로드 시각 → 정렬 가능한 파일명 prefix ("20260411_153042_")
// 업로드 순서대로 OS 기본 정렬 시에도 올바르게 배치되도록
QString uploadOrderPrefix(const QDateTime &uploadTime);
QString uploadOrderPrefix(qint64 unixSec);  // unix timestamp

// 타입별 하위 폴더 경로 (공통 정책)
//   baseDir: 플랫폼 루트 (예: /path/twitter/@shio)
//   type: 수집 타입 (tweets, replies, likes, reposts, followers, ...)
//   → baseDir/type/ 반환. 자동으로 mkpath
//   type이 "all"이면 baseDir/_complete/ 반환
QString typeFolder(const QString &baseDir, const QString &type);

// 타입별 Excel 파일 경로 (공통 정책)
//   excelDir: Excel이 저장되는 루트 (예: /path/twitter/@shio/excel)
//   type: 수집 타입
//   target: 대상 식별자 (유저명 등)
//   → excelDir/{target}_{type}.xlsx
//   type이 "all"이면 excelDir/{target}_complete.xlsx
QString typeExcelPath(const QString &excelDir, const QString &target, const QString &type);

// 메타데이터 일괄 수정: 파일 mtime/btime + macOS Finder 코멘트 + 다운로드 URL xattr
//   filePath: 대상 파일
//   uploadTime: 업로드 시각 (UTC or local)
//   sourceUrl: 원본 URL (nullable)
//   finderComment: Finder 코멘트 (nullable — 보통 JSON 메타)
void applyPostMetadata(const QString &filePath,
                        const QDateTime &uploadTime,
                        const QString &sourceUrl = QString(),
                        const QString &finderComment = QString());

// ──────────────────────────────────────────────────────────
// 経済産業省 연계: 페이지 캡처 (SingleFile 스타일)
// ──────────────────────────────────────────────────────────

// 원본 URL에서 HTML을 가져와 CSS/이미지를 base64 인라인하여
// 단일 HTML 파일로 저장 (経済産業省 크롤러 방식)
//   saveDir: 저장 경로 (예: userDir/captures/)
//   url: 캡처할 URL
//   title: 제목 (파일명에 사용)
//   http: HttpClient 포인터 (nullptr이면 내부 생성)
//   headers: 요청 헤더 (쿠키 등)
//   returns: 저장된 파일 경로 (실패 시 빈 문자열)
QString capturePageHtml(const QString &saveDir,
                        const QString &url,
                        const QString &title = QString(),
                        void *http = nullptr,
                        const QMap<QString, QString> &headers = QMap<QString, QString>());

// ──────────────────────────────────────────────────────────
// 브라우저-렌더된 HTML을 받아서 그대로 저장 (JS SPA 대응)
// ──────────────────────────────────────────────────────────
//
// JS-heavy 사이트 (Twitter/Bluesky/asked.kr 등)는 HTTP GET으로 받으면 셸만
// 오므로 capturePageHtml이 저장을 스킵한다. QWebEngineView::page()->toHtml()
// 로 받은 렌더된 HTML을 이 함수에 넘기면, 사용자가 실제로 보는 화면 그대로
// 저장된다 (CSS/이미지 인라인은 best-effort).
//
//   saveDir:     저장 경로 (예: userDir/captures/)
//   url:         페이지 URL (메타데이터용)
//   title:       파일명 base (확장자 .html 자동)
//   renderedHtml: bv->page()->toHtml(callback) 콜백에서 받은 HTML 문자열
//   inlineHttp:  CSS/이미지 base64 인라인용 클라이언트 (nullptr이면 인라인 안함)
//   headers:     인라인 요청 헤더
//   returns:     저장된 파일 경로 (실패 시 빈 문자열)
QString capturePageHtmlFromContent(const QString &saveDir,
                                    const QString &url,
                                    const QString &title,
                                    const QString &renderedHtml,
                                    void *inlineHttp = nullptr,
                                    const QMap<QString, QString> &headers = QMap<QString, QString>());

// ──────────────────────────────────────────────────────────
// 트윗 JSON → 정적 아카이브 HTML (x.com JS 셸 대신 실제 내용 저장)
// ──────────────────────────────────────────────────────────
//
// x.com은 서버에서 로그아웃 상태의 JS 셸만 반환하므로 HTTP GET으로는
// "JavaScriptを使用できません" 페이지만 저장됨. 대신 graphql로 이미
// 받아온 트윗 JSON에서 필요한 데이터를 뽑아 정적 HTML 아카이브를 생성.
//
//   saveDir: 저장 경로 (예: userDir/captures)
//   filename: 확장자 없는 기본 파일명
//   meta: 다음 키를 담은 QJsonObject
//     - authorName, handle, tweetId, tweetUrl, tweetText,
//       createdAt, tweetType, favoriteCount, retweetCount, replyCount
//     - mediaRelPaths: QJsonArray of string (saveDir 기준 상대경로)
//   returns: 저장된 파일 경로 (실패 시 빈 문자열)
QString generateTweetArchiveHtml(const QString &saveDir,
                                  const QString &filename,
                                  const QJsonObject &meta);

} // namespace FileHelper
