/* 原生界面的模型读写及数值程序桥接。单位：m、kN、kN/m²、m²。
 * 求解只调用同目录的独立 C 程序，不复制刚度算法；不使用 shell。
 * 所有用户文件路径使用 Win32 宽字符接口，数字文件采用 ASCII 字段。
 */
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include "truss_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define UI_BLOB_LIMIT (64L * 1024L * 1024L)
typedef struct { FILE *file; int line, token_line; wchar_t *error; size_t cap; } Reader;

static BOOL fail(wchar_t *error, size_t cap, const wchar_t *format, ...) {
    if (error && cap) {
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(error, cap, _TRUNCATE, format, args);
        va_end(args);
        error[cap - 1] = L'\0';
    }
    return FALSE;
}
static void clear_error(wchar_t *error, size_t cap) { if (error && cap) error[0] = 0; }

#ifdef TRUSS_STANDALONE
/* 每次启动独占一个 bundle，文件均采用 CREATE_NEW，清理范围固定。 */
typedef struct {
    wchar_t root[MAX_PATH], program[MAX_PATH], inputs[MAX_PATH];
    wchar_t core[MAX_PATH], lecture[MAX_PATH], seven[MAX_PATH];
    BOOL root_owned, program_owned, inputs_owned;
    BOOL core_owned, lecture_owned, seven_owned;
} StandaloneBundle;
static StandaloneBundle standalone_bundle;

static void standalone_delete_file(const wchar_t *path, BOOL *owned) {
    if (*owned && (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND))
        *owned = FALSE;
}
static void standalone_delete_directory(const wchar_t *path, BOOL *owned) {
    if (*owned && (RemoveDirectoryW(path) || GetLastError() == ERROR_PATH_NOT_FOUND))
        *owned = FALSE;
}
void ui_release_standalone(void) {
    StandaloneBundle *bundle = &standalone_bundle;
    standalone_delete_file(bundle->core, &bundle->core_owned);
    standalone_delete_file(bundle->lecture, &bundle->lecture_owned);
    standalone_delete_file(bundle->seven, &bundle->seven_owned);
    standalone_delete_directory(bundle->program, &bundle->program_owned);
    standalone_delete_directory(bundle->inputs, &bundle->inputs_owned);
    standalone_delete_directory(bundle->root, &bundle->root_owned);
    if (!bundle->root_owned) memset(bundle, 0, sizeof(*bundle));
}
static BOOL standalone_path(wchar_t *path, const wchar_t *parent,
                            const wchar_t *name, wchar_t *error, size_t cap) {
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", parent, name) < 0)
        return fail(error, cap, L"临时资源路径过长，请缩短系统临时目录路径。");
    return TRUE;
}
static BOOL standalone_write_resource(HINSTANCE instance, WORD id,
                                      const wchar_t *path, BOOL *owned,
                                      wchar_t *error, size_t cap) {
    HRSRC resource;
    HGLOBAL loaded;
    const unsigned char *data;
    DWORD bytes, offset = 0;
    HANDLE file;
    resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource)
        return fail(error, cap, L"内置资源 %u 缺失，请使用完整的单文件程序。", (unsigned)id);
    bytes = SizeofResource(instance, resource);
    loaded = LoadResource(instance, resource);
    data = loaded ? (const unsigned char *)LockResource(loaded) : NULL;
    if (!bytes || !data)
        return fail(error, cap, L"无法读取内置资源 %u。", (unsigned)id);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return fail(error, cap, L"无法创建临时运行资源（Windows 错误 %lu）。",
                    (unsigned long)GetLastError());
    *owned = TRUE;
    while (offset < bytes) {
        DWORD written = 0;
        if (!WriteFile(file, data + offset, bytes - offset, &written, NULL) || !written) {
            DWORD code = GetLastError();
            CloseHandle(file);
            return fail(error, cap, L"临时运行资源未写入完整（Windows 错误 %lu）。",
                        (unsigned long)code);
        }
        offset += written;
    }
    if (!CloseHandle(file))
        return fail(error, cap, L"无法关闭临时运行资源（Windows 错误 %lu）。",
                    (unsigned long)GetLastError());
    return TRUE;
}
BOOL ui_prepare_standalone(HINSTANCE instance, wchar_t *program_dir, size_t cap,
                          wchar_t *error, size_t errorcap) {
    StandaloneBundle *bundle = &standalone_bundle;
    wchar_t temporary[MAX_PATH], candidate[MAX_PATH];
    DWORD length;
    size_t output_length;
    clear_error(error, errorcap);
    if (!program_dir || !cap)
        return fail(error, errorcap, L"单文件程序的资源路径输出缓冲区无效。");
    program_dir[0] = L'\0';
    if (bundle->root_owned)
        return fail(error, errorcap, L"单文件程序的运行资源已经初始化。");
    memset(bundle, 0, sizeof(*bundle));
    length = GetTempPathW(MAX_PATH, temporary);
    if (!length || length >= MAX_PATH)
        return fail(error, errorcap, L"无法读取系统临时目录，或临时目录路径过长。");
    /* 占用随机名称后原子创建目录；创建失败时不清理其他进程的目录。 */
    if (!GetTempFileNameW(temporary, L"pst", 0, candidate))
        return fail(error, errorcap, L"无法分配独立临时目录（Windows 错误 %lu）。",
                    (unsigned long)GetLastError());
    if (!DeleteFileW(candidate))
        return fail(error, errorcap, L"无法准备临时目录（Windows 错误 %lu）。",
                    (unsigned long)GetLastError());
    if (!CreateDirectoryW(candidate, NULL))
        return fail(error, errorcap, L"无法创建独立临时目录（Windows 错误 %lu）。",
                    (unsigned long)GetLastError());
    wcscpy_s(bundle->root, MAX_PATH, candidate);
    bundle->root_owned = TRUE;
    if (!standalone_path(bundle->program, bundle->root, L"程序", error, errorcap) ||
        !standalone_path(bundle->inputs, bundle->root, L"总控信息", error, errorcap) ||
        !standalone_path(bundle->core, bundle->program, L"truss.exe", error, errorcap) ||
        !standalone_path(bundle->lecture, bundle->inputs, L"课件四杆_验证算例.txt", error, errorcap) ||
        !standalone_path(bundle->seven, bundle->inputs, L"七杆桁架_验证算例.txt", error, errorcap))
        goto failed;
    output_length = wcslen(bundle->program) + 1;
    if (output_length > cap) {
        fail(error, errorcap, L"单文件程序的资源路径输出缓冲区过小。");
        goto failed;
    }
    if (!CreateDirectoryW(bundle->program, NULL)) {
        fail(error, errorcap, L"无法创建临时程序目录（Windows 错误 %lu）。",
             (unsigned long)GetLastError());
        goto failed;
    }
    bundle->program_owned = TRUE;
    if (!CreateDirectoryW(bundle->inputs, NULL)) {
        fail(error, errorcap, L"无法创建临时算例目录（Windows 错误 %lu）。",
             (unsigned long)GetLastError());
        goto failed;
    }
    bundle->inputs_owned = TRUE;
    if (!standalone_write_resource(instance, 501, bundle->core, &bundle->core_owned, error, errorcap) ||
        !standalone_write_resource(instance, 502, bundle->lecture, &bundle->lecture_owned, error, errorcap) ||
        !standalone_write_resource(instance, 503, bundle->seven, &bundle->seven_owned, error, errorcap))
        goto failed;
    wmemcpy(program_dir, bundle->program, output_length);
    return TRUE;
failed:
    ui_release_standalone();
    return FALSE;
}
#endif

static BOOL input_fail(Reader *r, const wchar_t *field, const wchar_t *reason) {
    return fail(r->error, r->cap, L"输入第 %d 行，字段 %ls：%ls。", r->token_line, field, reason);
}
static UIModel *new_model(int n, int m) {
    UIModel *a = (UIModel *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->n = n; a->m = m;
    a->nodes = (UINode *)calloc((size_t)n, sizeof(*a->nodes));
    a->elements = (UIElement *)calloc((size_t)m, sizeof(*a->elements));
    if (!a->nodes || !a->elements) { ui_free_model(a); return NULL; }
    return a;
}
void ui_free_model(UIModel *a) {
    if (!a) return;
    free(a->nodes); free(a->elements); free(a->raw_output); free(a);
}
UIModel *ui_clone_model(const UIModel *source) {
    UIModel *a;
    if (!source || source->n < 2 || source->n > 1000 || source->m < 1 ||
        source->m > 10000 || !source->nodes || !source->elements) return NULL;
    a = new_model(source->n, source->m);
    if (!a) return NULL;
    {
        UINode *nodes = a->nodes; UIElement *elements = a->elements;
        *a = *source; a->nodes = nodes; a->elements = elements; a->raw_output = NULL;
    }
    memcpy(a->nodes, source->nodes, (size_t)a->n * sizeof(*a->nodes));
    memcpy(a->elements, source->elements, (size_t)a->m * sizeof(*a->elements));
    if (source->raw_output) {
        size_t bytes = strlen(source->raw_output) + 1;
        a->raw_output = (char *)malloc(bytes);
        if (!a->raw_output) { ui_free_model(a); return NULL; }
        memcpy(a->raw_output, source->raw_output, bytes);
    }
    return a;
}
/* 清理上次结果，保留已验证的坐标、杆长及材料信息。 */
static void clear_results(UIModel *a) {
    int i;
    a->solved = 0;
    free(a->raw_output); a->raw_output = NULL;
    a->max_displacement = a->max_axial = a->residual = a->relative_residual = 0;
    a->energy = a->work = a->energy_error = 0;
    memset(a->balance, 0, sizeof(a->balance));
    if (a->nodes && a->n >= 0 && a->n <= 1000)
        for (i = 0; i < a->n; ++i) a->nodes[i].ux = a->nodes[i].uy = a->nodes[i].rx = a->nodes[i].ry = 0;
    if (a->elements && a->m >= 0 && a->m <= 10000)
        for (i = 0; i < a->m; ++i) a->elements[i].axial = a->elements[i].extension = a->elements[i].stress = 0;
}
static int node_index(const UIModel *a, int id) {
    int i; for (i = 0; i < a->n; ++i) if (a->nodes[i].id == id) return i;
    return -1;
}
static int element_index(const UIModel *a, int id) {
    int i; for (i = 0; i < a->m; ++i) if (a->elements[i].id == id) return i;
    return -1;
}
static BOOL valid_model(const UIModel *a, wchar_t *error, size_t cap) {
    int i, j;
    if (!a || a->n < 2 || a->n > 1000 || a->m < 1 || a->m > 10000 || !a->nodes || !a->elements)
        return fail(error, cap, L"模型不完整：节点数需为 2..1000，杆件数需为 1..10000。");
    for (i = 0; i < a->n; ++i) {
        const UINode *n = &a->nodes[i];
        if (!isfinite(n->x) || !isfinite(n->y) || !isfinite(n->fx) || !isfinite(n->fy) ||
            !isfinite(n->prescribed_ux) || !isfinite(n->prescribed_uy))
            return fail(error, cap, L"节点 %d 的坐标、荷载或指定位移不是有限数。", n->id);
        if ((n->fix_x != 0 && n->fix_x != 1) || (n->fix_y != 0 && n->fix_y != 1))
            return fail(error, cap, L"节点 %d 的约束标志只能为 0 或 1。", n->id);
        if ((!n->fix_x && n->prescribed_ux != 0) || (!n->fix_y && n->prescribed_uy != 0))
            return fail(error, cap, L"节点 %d 的自由方向不能指定非零位移。", n->id);
        for (j = 0; j < i; ++j) if (a->nodes[j].id == n->id)
            return fail(error, cap, L"节点编号 %d 重复。", n->id);
    }
    for (i = 0; i < a->m; ++i) {
        const UIElement *e = &a->elements[i];
        if (e->ni < 0 || e->ni >= a->n || e->nj < 0 || e->nj >= a->n)
            return fail(error, cap, L"杆件 %d 引用了不存在的节点。", e->id);
        if (!(e->E > 0) || !(e->A > 0) || !isfinite(e->E) || !isfinite(e->A))
            return fail(error, cap, L"杆件 %d 的 E、A 必须为正的有限数。", e->id);
        for (j = 0; j < i; ++j) if (a->elements[j].id == e->id)
            return fail(error, cap, L"杆件编号 %d 重复。", e->id);
    }
    return TRUE;
}
/* 只在加载/创建模型时预计算几何，后续图形及结果校验直接复用。 */
static BOOL prepare_model(UIModel *a, wchar_t *error, size_t cap) {
    int i;
    if (!valid_model(a, error, cap)) return FALSE;
    a->free_dof = 0;
    for (i = 0; i < a->n; ++i) a->free_dof += !a->nodes[i].fix_x + !a->nodes[i].fix_y;
    for (i = 0; i < a->m; ++i) {
        UIElement *e = &a->elements[i];
        double dx = a->nodes[e->nj].x - a->nodes[e->ni].x;
        double dy = a->nodes[e->nj].y - a->nodes[e->ni].y;
        e->length = hypot(dx, dy);
        if (e->ni == e->nj || !(e->length > 0) || !isfinite(e->length))
            return fail(error, cap, L"杆件 %d 的杆长必须为正的有限数，不能连接同一节点。", e->id);
        e->c = dx / e->length; e->s = dy / e->length;
        if (!isfinite(e->E * e->A / e->length))
            return fail(error, cap, L"杆件 %d 的 EA/L 发生数值溢出，请检查单位和参数。", e->id);
    }
    return TRUE;
}
UIModel *ui_required_model(double l, double h, wchar_t *error, size_t cap) {
    const int ends[6][2] = {{0,3},{2,3},{1,3},{0,2},{2,1},{0,1}};
    UIModel *a; int i;
    clear_error(error, cap);
    if (!(l > 0) || !isfinite(l)) { fail(error, cap, L"底杆长度 l 必须为正的有限数，单位 m。"); return NULL; }
    if (!isfinite(h) || !(h > l / (2 * sqrt(3.0)))) {
        fail(error, cap, L"顶点高度 h 必须大于 l/(2√3)，使节点 4 位于节点 3 上方，单位 m。"); return NULL;
    }
    a = new_model(4, 6);
    if (!a) { fail(error, cap, L"内存不足，无法创建模型。"); return NULL; }
    a->required = 1; a->l = l; a->h = h;
    wcscpy_s(a->name, 160, L"必做题：六杆平面桁架（l、h 为输入参数）");
    for (i = 0; i < 4; ++i) a->nodes[i].id = i + 1;
    a->nodes[1].x = l;
    a->nodes[2].x = a->nodes[3].x = l / 2;
    a->nodes[2].y = l / (2 * sqrt(3.0)); a->nodes[3].y = h;
    for (i = 0; i < 2; ++i) a->nodes[i].fix_x = a->nodes[i].fix_y = 1;
    a->nodes[3].fx = 20 + 100 / sqrt(2.0); a->nodes[3].fy = -100 / sqrt(2.0);
    for (i = 0; i < 6; ++i) {
        UIElement *e = &a->elements[i];
        e->id = i + 1; e->ni = ends[i][0]; e->nj = ends[i][1]; e->E = 2e7; e->A = 0.24;
    }
    if (!prepare_model(a, error, cap)) { ui_free_model(a); return NULL; }
    return a;
}
/* token 返回 1=成功、0=文件结束、-1=错误；记录词首行号。 */
static int next_token(Reader *r, char b[128]) {
    int ch, len = 0;
    for (;;) {
        ch = fgetc(r->file);
        if (ch == EOF) {
            r->token_line = r->line;
            if (ferror(r->file)) { input_fail(r, L"读取", L"文件读取失败"); return -1; }
            return 0;
        }
        if (ch == '#') { while (ch != EOF && ch != '\n') ch = fgetc(r->file); }
        if (ch == '\n') ++r->line;
        if (ch != EOF && !isspace((unsigned char)ch)) break;
    }
    r->token_line = r->line;
    do {
        if (ch == 0 || ch > 127) { input_fail(r, L"数据词", L"数字和关键字必须使用 ASCII 字符"); return -1; }
        if (len == 127) { input_fail(r, L"数据词", L"长度超过 127 字节"); return -1; }
        b[len++] = (char)ch; ch = fgetc(r->file);
    } while (ch != EOF && !isspace((unsigned char)ch) && ch != '#');
    if (ch == '#') while (ch != EOF && ch != '\n') ch = fgetc(r->file);
    if (ch == '\n') ++r->line;
    b[len] = 0; return 1;
}
static BOOL read_number(Reader *r, const wchar_t *field, double *value) {
    char b[128], *end; int status = next_token(r, b);
    if (status < 0) return FALSE;
    if (!status) return input_fail(r, field, L"缺少数值");
    errno = 0; *value = strtod(b, &end);
    if (errno || end == b || *end || !isfinite(*value)) return input_fail(r, field, L"需填写有限数，不能使用 NaN/Inf");
    return TRUE;
}
static BOOL read_integer(Reader *r, const wchar_t *field, int *value) {
    double v;
    if (!read_number(r, field, &v)) return FALSE;
    if (v < INT_MIN || v > INT_MAX || floor(v) != v) return input_fail(r, field, L"需填写整数");
    *value = (int)v; return TRUE;
}
static BOOL read_key(Reader *r, const char *key) {
    char b[128]; int status = next_token(r, b);
    if (status < 0) return FALSE;
    if (!status || strcmp(b, key)) return fail(r->error, r->cap, L"输入第 %d 行：缺少或错误的关键字 %hs。", r->token_line, key);
    return TRUE;
}
UIModel *ui_load_model(const wchar_t *path, wchar_t *error, size_t cap) {
    FILE *f = NULL; UIModel *a = NULL; Reader r; int n, m, version, i, j, first; char b[128];
    clear_error(error, cap);
    if (!path || !*path || !(f = _wfopen(path, L"rb"))) { fail(error, cap, L"无法打开总控信息文件，请检查路径和读取权限。"); return NULL; }
    r.file = f; r.line = r.token_line = 1; r.error = error; r.cap = cap;
    first = fgetc(f);
    if (first == 0xEF) {
        if (fgetc(f) != 0xBB || fgetc(f) != 0xBF) { input_fail(&r, L"编码", L"UTF-8 BOM 不完整"); goto bad; }
    } else if (first != EOF) ungetc(first, f);
    if (!read_key(&r, "TRUSS2D") || !read_integer(&r, L"版本", &version)) goto bad;
    if (version != 1) { input_fail(&r, L"版本", L"仅支持 TRUSS2D 1"); goto bad; }
    if (!read_key(&r, "NODES") || !read_integer(&r, L"节点数量", &n)) goto bad;
    if (n < 2 || n > 1000) { input_fail(&r, L"节点数量", L"范围需为 2..1000"); goto bad; }
    a = new_model(n, 1);
    if (!a) { fail(error, cap, L"内存不足，无法加载模型。"); goto bad; }
    for (i = 0; i < n; ++i) {
        UINode *u = &a->nodes[i];
        if (!read_integer(&r, L"节点编号", &u->id)) goto bad;
        for (j = 0; j < i; ++j) if (a->nodes[j].id == u->id) { input_fail(&r, L"节点编号", L"编号重复"); goto bad; }
        if (!read_number(&r, L"x", &u->x) || !read_number(&r, L"y", &u->y) ||
            !read_integer(&r, L"fix_x", &u->fix_x) || !read_integer(&r, L"fix_y", &u->fix_y)) goto bad;
        if ((u->fix_x != 0 && u->fix_x != 1) || (u->fix_y != 0 && u->fix_y != 1)) {
            input_fail(&r, L"fix_x/fix_y", L"约束标志只能为 0 或 1"); goto bad;
        }
        if (!read_number(&r, L"prescribed_ux", &u->prescribed_ux) || !read_number(&r, L"prescribed_uy", &u->prescribed_uy) ||
            !read_number(&r, L"Fx", &u->fx) || !read_number(&r, L"Fy", &u->fy)) goto bad;
        if ((!u->fix_x && u->prescribed_ux != 0) || (!u->fix_y && u->prescribed_uy != 0)) {
            input_fail(&r, L"指定位移", L"自由方向的指定位移必须为 0"); goto bad;
        }
    }
    if (!read_key(&r, "ELEMENTS") || !read_integer(&r, L"杆件数量", &m)) goto bad;
    if (m < 1 || m > 10000) { input_fail(&r, L"杆件数量", L"范围需为 1..10000"); goto bad; }
    free(a->elements); a->elements = (UIElement *)calloc((size_t)m, sizeof(*a->elements)); a->m = m;
    if (!a->elements) { fail(error, cap, L"内存不足，无法加载杆件。"); goto bad; }
    for (i = 0; i < m; ++i) {
        UIElement *e = &a->elements[i]; int ni, nj;
        if (!read_integer(&r, L"杆件编号", &e->id)) goto bad;
        for (j = 0; j < i; ++j) if (a->elements[j].id == e->id) { input_fail(&r, L"杆件编号", L"编号重复"); goto bad; }
        if (!read_integer(&r, L"node_i", &ni) || !read_integer(&r, L"node_j", &nj)) goto bad;
        e->ni = node_index(a, ni); e->nj = node_index(a, nj);
        if (e->ni < 0 || e->nj < 0) { input_fail(&r, L"杆端节点编号", L"引用了不存在的节点"); goto bad; }
        if (!read_number(&r, L"E", &e->E) || !read_number(&r, L"A", &e->A)) goto bad;
        if (!(e->E > 0) || !(e->A > 0)) { input_fail(&r, L"E/A", L"弹性模量及截面积必须为正数"); goto bad; }
    }
    if (!read_key(&r, "END")) goto bad;
    first = next_token(&r, b);
    if (first < 0) goto bad;
    if (first > 0) { input_fail(&r, L"END", L"结束标记后存在多余数据"); goto bad; }
    if (ferror(f)) { fail(error, cap, L"总控信息文件读取失败。"); goto bad; }
    fclose(f); f = NULL;
    if (!prepare_model(a, error, cap)) goto bad;
    {
        const wchar_t *base = path, *p;
        for (p = path; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
        wcsncpy_s(a->name, 160, base, _TRUNCATE);
    }
    return a;
bad:
    if (f) fclose(f);
    ui_free_model(a); return NULL;
}
BOOL ui_save_model(const UIModel *a, const wchar_t *path, wchar_t *error, size_t cap) {
    FILE *f; int i; BOOL ok;
    clear_error(error, cap);
    if (!valid_model(a, error, cap)) return FALSE;
    if (!path || !*path || !(f = _wfopen(path, L"wb"))) return fail(error, cap, L"无法写入总控信息文件，请检查路径和写入权限。");
    fprintf(f, "# Units: m, kN, kN/m^2, m^2. Prescribed displacements are in m.\n");
    if (a->required) fprintf(f, "# Required problem: l=%.17g m, h=%.17g m (user parameters).\n", a->l, a->h);
    fprintf(f, "TRUSS2D 1\nNODES %d\n# id x y fix_x fix_y prescribed_ux prescribed_uy Fx Fy\n", a->n);
    for (i = 0; i < a->n; ++i) {
        const UINode *n = &a->nodes[i];
        fprintf(f, "%d %.17g %.17g %d %d %.17g %.17g %.17g %.17g\n", n->id, n->x, n->y,
            n->fix_x, n->fix_y, n->prescribed_ux, n->prescribed_uy, n->fx, n->fy);
    }
    fprintf(f, "ELEMENTS %d\n# id node_i node_j E A\n", a->m);
    for (i = 0; i < a->m; ++i) {
        const UIElement *e = &a->elements[i];
        fprintf(f, "%d %d %d %.17g %.17g\n", e->id, a->nodes[e->ni].id, a->nodes[e->nj].id, e->E, e->A);
    }
    fprintf(f, "END\n"); ok = !ferror(f);
    if (fclose(f)) ok = FALSE;
    return ok ? TRUE : fail(error, cap, L"总控信息写入失败，文件可能未完整保存。");
}
static char *read_blob(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb"); __int64 bytes; char *b;
    if (!f) return NULL;
    if (_fseeki64(f, 0, SEEK_END) || (bytes = _ftelli64(f)) < 0 || bytes > UI_BLOB_LIMIT || _fseeki64(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)bytes + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)bytes, f) != (size_t)bytes || ferror(f)) { free(b); fclose(f); return NULL; }
    b[(size_t)bytes] = 0;
    if (memchr(b, 0, (size_t)bytes)) { free(b); fclose(f); return NULL; }
    fclose(f); return b;
}
/* Windows 标准 argv 引号规则，处理内含引号及引号前/末尾的反斜杠。 */
static wchar_t *quoted_command(const wchar_t *exe, const wchar_t *input, const wchar_t *output) {
    const wchar_t *args[3] = {exe, input, output}; size_t total = 8, pos = 0; int i;
    wchar_t *cmd;
    for (i = 0; i < 3; ++i) total += 2 * wcslen(args[i]) + 4;
    if (total > 32767) return NULL;
    cmd = (wchar_t *)calloc(total, sizeof(wchar_t));
    if (!cmd) return NULL;
    for (i = 0; i < 3; ++i) {
        const wchar_t *p = args[i]; size_t slashes;
        if (i) cmd[pos++] = L' ';
        cmd[pos++] = L'"';
        while (*p) {
            slashes = 0;
            while (*p == L'\\') { ++slashes; ++p; }
            if (*p == L'"') {
                size_t count = 2 * slashes + 1;
                while (count--) cmd[pos++] = L'\\';
                cmd[pos++] = *p++;
            } else if (!*p) {
                size_t count = 2 * slashes;
                while (count--) cmd[pos++] = L'\\';
            } else {
                while (slashes--) cmd[pos++] = L'\\';
                cmd[pos++] = *p++;
            }
        }
        cmd[pos++] = L'"';
    }
    cmd[pos] = 0; return cmd;
}
static void translate_core_error(const char *raw, DWORD code, wchar_t *error, size_t cap) {
    const wchar_t *reason = L"求解器未完成计算，请检查总控信息和程序文件。";
    wchar_t detail[1600] = L"";
    if (raw) {
        if (strstr(raw, "not positive definite") || strstr(raw, "zero or invalid stiffness"))
            reason = L"结构约束不足、存在机构或严重病态，请检查支座方向及杆件连接。";
        else if (strstr(raw, "verification failed")) reason = L"数值平衡校验未通过，请检查结构稳定性及参数量级。";
        else if (strstr(raw, "overflow")) reason = L"数值溢出，请检查单位及 E、A、尺寸和荷载的量级。";
        else if (strstr(raw, "Cannot open")) reason = L"求解器无法打开临时输入或输出文件，请检查临时目录权限。";
        else if (strstr(raw, "memory")) reason = L"求解器内存不足，请减少模型规模或关闭其他大型程序。";
        else if (strstr(raw, "length")) reason = L"存在零长度或无效杆件，请检查节点坐标及连接。";
        else if (strstr(raw, "prescribed")) reason = L"自由方向不能指定非零位移，请检查约束数据。";
        MultiByteToWideChar(CP_UTF8, 0, raw, -1, detail, (int)(sizeof(detail)/sizeof(detail[0])));
        detail[sizeof(detail)/sizeof(detail[0])-1] = 0;
    }
    fail(error, cap, L"%ls\n退出码 %lu。%ls", reason, (unsigned long)code, detail);
}
static char *next_line(char **cursor) {
    char *start = *cursor, *end; size_t n;
    if (!start || !*start) return NULL;
    end = strchr(start, '\n');
    if (end) { *end = 0; *cursor = end + 1; } else *cursor = start + strlen(start);
    n = strlen(start); if (n && start[n-1] == '\r') start[n-1] = 0;
    return start;
}
static BOOL trailing_space(const char *p) { while (*p && isspace((unsigned char)*p)) ++p; return !*p; }
static BOOL finite4(double a, double b, double c, double d) { return isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d); }
/* 不用 scanf 的整型转换，避免损坏输出里的超范围整数发生未定义转换。 */
static BOOL line_numbers(const char *line, double *values, int count) {
    int i; const char *p = line; char *end;
    for (i = 0; i < count; ++i) {
        while (*p && isspace((unsigned char)*p)) ++p;
        errno = 0; values[i] = strtod(p, &end);
        if (errno || end == p || !isfinite(values[i]) || (*end && !isspace((unsigned char)*end))) return FALSE;
        p = end;
    }
    return trailing_space(p);
}
static BOOL output_integer(double value, int *result) {
    if (!isfinite(value) || value < INT_MIN || value > INT_MAX || floor(value) != value) return FALSE;
    *result = (int)value; return TRUE;
}
static BOOL metadata_number(const char **cursor, const char *key, int *result) {
    size_t len = strlen(key); char *end; long value;
    if (strncmp(*cursor, key, len)) return FALSE;
    errno = 0; value = strtol(*cursor + len, &end, 10);
    if (errno || end == *cursor+len || value < INT_MIN || value > INT_MAX || (*end && !isspace((unsigned char)*end))) return FALSE;
    *result = (int)value;
    while (*end && isspace((unsigned char)*end)) ++end;
    *cursor = end; return TRUE;
}
/* 先解析到工作模型；全部数量/编号/有限性校验通过后才提交 solved。
 * 保留未改写的 raw_output，以保证导出精度等于独立程序原始输出。 */
static BOOL parse_results(UIModel *a, const char *raw, wchar_t *error, size_t cap) {
    static const char *keys[8] = {"max_free_residual(kN)","relative_free_residual","sum_Fx(kN)","sum_Fy(kN)",
        "sum_Mz(kN*m)","strain_energy(kN*m)","load_and_support_work(kN*m)","energy_identity_error(kN*m)"};
    char *copy = NULL, *cursor, *line; unsigned char *seen_n = NULL, *seen_e = NULL;
    double checks[8] = {0}; int metadata = 0, nodes = 0, elements = 0, check_mask = 0;
    int state = 0, header_n = 0, header_e = 0, header_c = 0, i, expected_nf = 0; BOOL ok = FALSE;
    size_t bytes = strlen(raw) + 1;
    for (i = 0; raw[i]; ++i) if ((unsigned char)raw[i] > 127 || ((unsigned char)raw[i] < 32 && raw[i] != '\n' && raw[i] != '\r' && raw[i] != '\t'))
        return fail(error, cap, L"求解器输出含无效字符，未接收结果。");
    copy = (char *)malloc(bytes); seen_n = (unsigned char *)calloc((size_t)a->n, 1); seen_e = (unsigned char *)calloc((size_t)a->m, 1);
    if (!copy || !seen_n || !seen_e) { fail(error, cap, L"内存不足，无法读取结果。"); goto done; }
    memcpy(copy, raw, bytes); cursor = copy;
    line = next_line(&cursor);
    if (!line || strcmp(line, "PLANE TRUSS - DIRECT STIFFNESS METHOD")) goto malformed;
    line = next_line(&cursor);
    if (!line || strcmp(line, "Units: m, kN, kN/m^2. Axial force: tension +, compression -.")) goto malformed;
    for (i = 0; i < a->n; ++i) expected_nf += !a->nodes[i].fix_x + !a->nodes[i].fix_y;
    while ((line = next_line(&cursor)) != NULL) {
        if (!*line) {
            if (state == 1) { if (nodes != a->n) goto malformed; state = 4; }
            if (state == 2) { if (elements != a->m) goto malformed; state = 5; }
            continue;
        }
        if (!strncmp(line, "Nodes=", 6)) {
            int n, m, nf; const char *p = line;
            if (metadata || !metadata_number(&p, "Nodes=", &n) || !metadata_number(&p, "Elements=", &m) ||
                !metadata_number(&p, "Free_DOF=", &nf) || !trailing_space(p) || n != a->n || m != a->m || nf != expected_nf) goto malformed;
            metadata = 1; a->free_dof = nf; continue;
        }
        if (!strcmp(line, "NODE RESULTS")) {
            if (header_n || !metadata) goto malformed;
            header_n = 1; state = 1; line = next_line(&cursor);
            if (!line || strcmp(line, "id ux(m) uy(m) Rx(kN) Ry(kN) fix_x fix_y")) goto malformed;
            continue;
        }
        if (!strcmp(line, "ELEMENT RESULTS")) {
            if (header_e || nodes != a->n || !header_n) goto malformed;
            header_e = 1; state = 2; line = next_line(&cursor);
            if (!line || strcmp(line, "id node_i node_j L(m) N(kN) extension(m) stress(kN/m^2)")) goto malformed;
            continue;
        }
        if (!strcmp(line, "CHECKS")) {
            if (header_c || elements != a->m || !header_e) goto malformed;
            header_c = 1; state = 3; continue;
        }
        if (state == 1) {
            int id, fx, fy, index; double ux, uy, rx, ry, v[7]; UINode *n;
            if (!line_numbers(line, v, 7) || !output_integer(v[0], &id) ||
                !output_integer(v[5], &fx) || !output_integer(v[6], &fy)) goto malformed;
            ux = v[1]; uy = v[2]; rx = v[3]; ry = v[4];
            index = node_index(a, id);
            if (index < 0 || seen_n[index]) goto malformed;
            n = &a->nodes[index];
            if (fx != n->fix_x || fy != n->fix_y || (!fx && rx != 0) || (!fy && ry != 0)) goto malformed;
            if ((fx && fabs(ux - n->prescribed_ux) > 1e-10 * fmax(1.0, fabs(n->prescribed_ux))) ||
                (fy && fabs(uy - n->prescribed_uy) > 1e-10 * fmax(1.0, fabs(n->prescribed_uy)))) goto malformed;
            n->ux = ux; n->uy = uy; n->rx = rx; n->ry = ry;
            a->max_displacement = fmax(a->max_displacement, hypot(ux, uy));
            if (!isfinite(a->max_displacement)) goto malformed;
            seen_n[index] = 1; ++nodes;
        } else if (state == 2) {
            int id, ni, nj, index; double l, axial, ext, stress, v[7]; UIElement *e;
            if (!line_numbers(line, v, 7) || !output_integer(v[0], &id) ||
                !output_integer(v[1], &ni) || !output_integer(v[2], &nj)) goto malformed;
            l = v[3]; axial = v[4]; ext = v[5]; stress = v[6];
            if (!finite4(l, axial, ext, stress) || !(l > 0)) goto malformed;
            index = element_index(a, id);
            if (index < 0 || seen_e[index]) goto malformed;
            e = &a->elements[index];
            if (ni != a->nodes[e->ni].id || nj != a->nodes[e->nj].id ||
                fabs(l - e->length) > 1e-10 * fmax(l, e->length)) goto malformed;
            e->axial = axial; e->extension = ext; e->stress = stress;
            a->max_axial = fmax(a->max_axial, fabs(axial)); seen_e[index] = 1; ++elements;
        } else if (state == 3) {
            char *equals = strchr(line, '='), *end; double value; int key;
            if (!equals) goto malformed;
            *equals = 0;
            for (key = 0; key < 8; ++key) if (!strcmp(line, keys[key])) break;
            if (key == 8 || (check_mask & (1 << key))) goto malformed;
            errno = 0; value = strtod(equals+1, &end);
            if (errno || end == equals+1 || !trailing_space(end) || !isfinite(value)) goto malformed;
            checks[key] = value; check_mask |= 1 << key;
        } else if (state == 4 || state == 5) goto malformed;
        /* 原始自由度表、几何表和教学刚度表保留在原文，不重复求解。 */
    }
    if (!metadata || !header_n || !header_e || !header_c || nodes != a->n || elements != a->m || check_mask != 255 ||
        checks[0] < 0 || checks[1] < 0 || checks[1] > 1e-8 || checks[5] < 0) goto malformed;
    a->residual = checks[0]; a->relative_residual = checks[1];
    a->balance[0] = checks[2]; a->balance[1] = checks[3]; a->balance[2] = checks[4];
    a->energy = checks[5]; a->work = checks[6]; a->energy_error = checks[7]; ok = TRUE; goto done;
malformed:
    fail(error, cap, L"求解器结果不完整或与模型不一致（标题、数量、编号或数值校验失败），未接收结果。");
done:
    free(copy); free(seen_n); free(seen_e); return ok;
}
BOOL ui_solve_model(UIModel *a, const wchar_t *solver_path, wchar_t *error, size_t cap) {
    wchar_t temp[MAX_PATH], input[MAX_PATH] = L"", output[MAX_PATH] = L"", errpath[MAX_PATH] = L"";
    wchar_t *command = NULL, *exe = NULL; char *raw = NULL, *stderr_text = NULL;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE}; STARTUPINFOEXW si; PROCESS_INFORMATION pi;
    HANDLE herror = INVALID_HANDLE_VALUE, hnull = INVALID_HANDLE_VALUE; HANDLE inherited[2];
    SIZE_T attr_bytes = 0; DWORD path_len, code = 0, waited; UIModel *work = NULL; BOOL ok = FALSE, started = FALSE;
    BOOL own_input = FALSE, own_output = FALSE, own_error = FALSE;
    clear_error(error, cap);
    if (!a) return fail(error, cap, L"请先创建或打开模型。");
    clear_results(a);
    if (!valid_model(a, error, cap)) return FALSE;
    if (!solver_path || !*solver_path) return fail(error, cap, L"未指定同目录 truss.exe 求解器。");
    memset(&si, 0, sizeof(si)); memset(&pi, 0, sizeof(pi)); si.StartupInfo.cb = sizeof(si);
    path_len = GetFullPathNameW(solver_path, 0, NULL, NULL);
    if (!path_len || path_len > 32767 || !(exe = (wchar_t *)calloc((size_t)path_len+1, sizeof(wchar_t))) ||
        !GetFullPathNameW(solver_path, path_len+1, exe, NULL)) { fail(error, cap, L"求解器路径无效或内存不足。"); goto done; }
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { fail(error, cap, L"找不到 truss.exe，请将图形界面和求解器放在同一目录。"); goto done; }
    path_len = GetTempPathW(MAX_PATH, temp);
    if (!path_len || path_len >= MAX_PATH) {
        fail(error, cap, L"无法访问 Windows 临时目录，或其路径超过长度限制。"); goto done;
    }
    own_input = GetTempFileNameW(temp, L"tui", 0, input) != 0;
    if (own_input) own_output = GetTempFileNameW(temp, L"tuo", 0, output) != 0;
    if (own_output) own_error = GetTempFileNameW(temp, L"tue", 0, errpath) != 0;
    if (!own_error) { fail(error, cap, L"无法创建计算临时文件，请检查 Windows 临时目录和可用空间。"); goto done; }
    if (!ui_save_model(a, input, error, cap)) goto done;
    herror = CreateFileW(errpath, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    hnull = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (herror == INVALID_HANDLE_VALUE || hnull == INVALID_HANDLE_VALUE) { fail(error, cap, L"无法建立求解器输入输出通道。"); goto done; }
    command = quoted_command(exe, input, output);
    if (!command) { fail(error, cap, L"求解器参数路径过长或内存不足。"); goto done; }
    /* 继承白名单仅含 NUL 与 stderr；不泄露主程序的其他可继承句柄。 */
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_bytes);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_bytes);
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_bytes)) {
        free(si.lpAttributeList); si.lpAttributeList = NULL; fail(error, cap, L"无法建立安全的求解器进程配置。"); goto done;
    }
    inherited[0] = hnull; inherited[1] = herror;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), NULL, NULL)) {
        fail(error, cap, L"无法配置求解器输入输出句柄。"); goto done;
    }
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = hnull;
    si.StartupInfo.hStdOutput = hnull; si.StartupInfo.hStdError = herror;
    if (!CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        fail(error, cap, L"无法启动 truss.exe（Windows 错误 %lu），请检查程序文件。", (unsigned long)GetLastError()); goto done;
    }
    started = TRUE;
    waited = WaitForSingleObject(pi.hProcess, 120000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 3); WaitForSingleObject(pi.hProcess, 5000);
        fail(error, cap, waited == WAIT_TIMEOUT ? L"计算超过 120 秒，已停止求解器。请减少模型规模后重试。" : L"等待求解器失败，计算已停止。"); goto done;
    }
    if (!GetExitCodeProcess(pi.hProcess, &code)) { fail(error, cap, L"无法读取求解器退出状态。"); goto done; }
    CloseHandle(herror); herror = INVALID_HANDLE_VALUE;
    if (code) { stderr_text = read_blob(errpath); translate_core_error(stderr_text, code, error, cap); goto done; }
    raw = read_blob(output);
    if (!raw || !*raw) { fail(error, cap, L"求解器没有生成可读取的完整结果。"); goto done; }
    work = ui_clone_model(a);
    if (!work) { fail(error, cap, L"内存不足，无法接收计算结果。"); goto done; }
    if (!parse_results(work, raw, error, cap)) goto done;
    memcpy(a->nodes, work->nodes, (size_t)a->n * sizeof(*a->nodes));
    memcpy(a->elements, work->elements, (size_t)a->m * sizeof(*a->elements));
    a->free_dof = work->free_dof; a->max_displacement = work->max_displacement; a->max_axial = work->max_axial;
    a->residual = work->residual; a->relative_residual = work->relative_residual;
    a->energy = work->energy; a->work = work->work; a->energy_error = work->energy_error;
    memcpy(a->balance, work->balance, sizeof(a->balance)); a->raw_output = raw; raw = NULL; a->solved = 1; ok = TRUE;
done:
    if (started) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    if (si.lpAttributeList) { DeleteProcThreadAttributeList(si.lpAttributeList); free(si.lpAttributeList); }
    if (herror != INVALID_HANDLE_VALUE) CloseHandle(herror);
    if (hnull != INVALID_HANDLE_VALUE) CloseHandle(hnull);
    /* 只删除本次 GetTempFileName 创建的三个文件，不递归删除任何目录。 */
    if (own_input) DeleteFileW(input);
    if (own_output) DeleteFileW(output);
    if (own_error) DeleteFileW(errpath);
    free(exe); free(command); free(raw); free(stderr_text); ui_free_model(work);
    return ok;
}
BOOL ui_save_results(const UIModel *a, const wchar_t *path, wchar_t *error, size_t cap) {
    FILE *f; size_t bytes; BOOL ok;
    clear_error(error, cap);
    if (!a || !a->solved || !a->raw_output) return fail(error, cap, L"请先成功计算，再导出结果。");
    if (!path || !*path || !(f = _wfopen(path, L"wb"))) return fail(error, cap, L"无法写入结果文件，请检查路径和写入权限。");
    bytes = strlen(a->raw_output); ok = fwrite(a->raw_output, 1, bytes, f) == bytes && !ferror(f);
    if (fclose(f)) ok = FALSE;
    return ok ? TRUE : fail(error, cap, L"结果文件写入失败，文件可能未完整保存。");
}
