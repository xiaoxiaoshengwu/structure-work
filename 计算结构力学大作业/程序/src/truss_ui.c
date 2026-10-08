/* 平面桁架计算工作台：原生 Windows 图形界面。
 * 本文件负责窗口、输入状态、后台任务和结果表；数值算法仍在 truss.exe。
 * 模型长度单位 m，力单位 kN，表中位移换算成 mm；不改变求解精度。
 */
#include "truss_ui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_SOLVE_DONE (WM_APP + 11)
#define IDC_CANVAS 401
#define IDC_TITLE 402
#define IDC_SUBTITLE 403
#define IDC_MODEL_TITLE 404
#define IDC_MODEL_HINT 405
#define IDC_MODEL_INFO 406
#define IDC_LENGTH_LABEL 407
#define IDC_HEIGHT_LABEL 408
#define IDC_E_LABEL 409
#define IDC_A_LABEL 410
#define IDC_E_VALUE 411
#define IDC_A_VALUE 412
#define IDC_INPUT_NOTE 413
#define IDC_INPUT_ERROR 414
#define IDC_STRUCTURE_TITLE 415
#define IDC_RESULT_TITLE 416
#define IDC_RESULT_SUMMARY 417
#define IDC_RESULT_EMPTY 418
#define IDC_UNITS 419
#define IDC_SELECTION 420

typedef struct {
    HWND owner;
    UIModel *model;
    BOOL success;
    wchar_t solver[MAX_PATH], error[1024];
} SolveTask;

static HINSTANCE g_instance;
static HWND g_window, g_canvas, g_tab, g_table, g_raw, g_empty, g_status, g_tooltip;
static HFONT g_font, g_title_font, g_section_font, g_small_font, g_number_font, g_raw_font;
static HBRUSH g_bg_brush, g_surface_brush, g_panel_brush;
static HIMAGELIST g_row_images;
static HICON g_app_icon;
static int g_dpi = 96, g_preset = 0, g_active_tab = 0;
static BOOL g_updating = FALSE, g_input_valid = TRUE, g_busy = FALSE;
static BOOL g_close_pending = FALSE, g_selecting = FALSE;
static BOOL g_calculation_failed = FALSE;
static UIModel *g_model;
static UIView g_view = {0, -1, 1, 1.0, 0, 0};
static HANDLE g_thread;
static SolveTask *g_task;
static wchar_t g_program_dir[MAX_PATH], g_input_path[MAX_PATH];
#ifdef TRUSS_STANDALONE
static wchar_t g_export_dir[MAX_PATH];
#endif
static wchar_t g_model_tip[512];
static RECT g_left_panel, g_result_panel, g_view_group;
static int g_model_divider_y;

static int px(int value) { return MulDiv(value, g_dpi, 96); }
static HWND ctl(int id) { return GetDlgItem(g_window, id); }
static void set_text(int id, const wchar_t *text) { SetWindowTextW(ctl(id), text); }

/* 中英文混排统一用微软雅黑；标题与区块标题只是字号/字重不同。 */
static void create_fonts(void)
{
    if (g_font) DeleteObject(g_font);
    if (g_title_font) DeleteObject(g_title_font);
    if (g_section_font) DeleteObject(g_section_font);
    if (g_small_font) DeleteObject(g_small_font);
    if (g_number_font) DeleteObject(g_number_font);
    if (g_raw_font) DeleteObject(g_raw_font);
    g_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_title_font = CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_section_font = CreateFontW(-px(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_number_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Bahnschrift");
    g_raw_font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    g_small_font = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

static BOOL CALLBACK apply_font(HWND child, LPARAM unused)
{
    int id = GetDlgCtrlID(child);
    HFONT font = g_font;
    (void)unused;
    if (id == IDC_TITLE) font = g_title_font;
    if (id == IDC_MODEL_TITLE || id == IDC_STRUCTURE_TITLE || id == IDC_RESULT_TITLE)
        font = g_section_font;
    if (id == IDC_UNITS || id == IDC_SELECTION || id == IDC_RESULT_SUMMARY ||
        id == IDC_STATUS_TEXT || id == IDC_MODEL_INFO) font = g_small_font;
    if (id == IDC_LENGTH || id == IDC_HEIGHT || id == IDC_E_VALUE || id == IDC_A_VALUE)
        font = g_number_font;
    if (id == IDC_RAW_OUTPUT) font = g_raw_font;
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static HWND make_control(const wchar_t *class_name, const wchar_t *caption,
                         DWORD style, DWORD exstyle, int id)
{
    return CreateWindowExW(exstyle, class_name, caption, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 0, 0, g_window, (HMENU)(INT_PTR)id, g_instance, NULL);
}
static void move_control(int id, int x, int y, int w, int h)
{
    MoveWindow(ctl(id), x, y, w > 0 ? w : 1, h > 0 ? h : 1, TRUE);
}
static void make_label(int id, const wchar_t *caption)
{ make_control(L"STATIC", caption, SS_LEFT, 0, id); }
static void make_button(int id, const wchar_t *caption)
{ make_control(L"BUTTON", caption, WS_TABSTOP | BS_OWNERDRAW, 0, id); }

/* 只有有限的完整数值才能成为输入；空文本和尾随字符不会被当作零。 */
static BOOL read_number(int id, double *value)
{
    wchar_t buffer[128], *end;
    const wchar_t *begin;
    GetWindowTextW(ctl(id), buffer, (int)(sizeof(buffer) / sizeof(buffer[0])));
    begin = buffer;
    while (iswspace(*begin)) ++begin;
    if (!*begin) return FALSE;
    *value = wcstod(begin, &end);
    if (end == begin) return FALSE;
    while (iswspace(*end)) ++end;
    return *end == 0 && isfinite(*value);
}

static void fit_view(void)
{
    g_view.zoom = 1.0;
    g_view.pan_x = g_view.pan_y = 0.0;
    InvalidateRect(g_canvas, NULL, FALSE);
}
static void refresh_results(void);
static void update_controls(void);
static void layout(void);
static void size_table_columns(void);
static void style_control_metrics(void);

static void discard_results(UIModel *model)
{
    if (!model) return;
    model->solved = 0;
    free(model->raw_output);
    model->raw_output = NULL;
    g_view.selected_element = -1;
}

static void set_status(const wchar_t *message)
{
    SetWindowTextW(g_status, message);
    InvalidateRect(g_status, NULL, TRUE);
}

/* EM_CHANGE 每次触发都会更新预览；无效输入保留最近的几何图，但立即
 * 屏蔽旧计算结果与导出，避免把新参数误当成旧结果对应的参数。 */
static void parameters_changed(void)
{
    double l = 0.0, h = 0.0;
    wchar_t error[512];
    UIModel *candidate;
    if (g_updating || g_busy || g_preset != 0) return;
    g_calculation_failed = FALSE;
    discard_results(g_model);
    g_input_valid = read_number(IDC_LENGTH, &l) && read_number(IDC_HEIGHT, &h);
    if (!g_input_valid) {
        wcscpy_s(error, 512, L"请输入完整的有限数值 l 与 h。");
    } else {
        candidate = ui_required_model(l, h, error, 512);
        if (candidate) {
            ui_free_model(g_model);
            g_model = candidate;
            g_input_path[0] = 0;
        } else g_input_valid = FALSE;
    }
    set_text(IDC_INPUT_ERROR, g_input_valid ? L"" : error);
    set_status(g_input_valid ? L"参数已更新" :
               L"输入有误 · 请检查参数");
    refresh_results();
    update_controls();
    InvalidateRect(g_canvas, NULL, FALSE);
}

static void refresh_model_info(void)
{
    wchar_t info[512], name[180];
    wchar_t material[96];
    BOOL uniform_e = TRUE, uniform_a = TRUE;
    int i;
    if (!g_model) return;
    if (g_preset == 0) wcscpy_s(name, 180, L"六杆对称桁架");
    else if (g_input_path[0]) {
        const wchar_t *base = wcsrchr(g_input_path, L'\\');
        wcsncpy_s(name, 180, base ? base + 1 : g_input_path, _TRUNCATE);
    } else wcsncpy_s(name, 180, g_model->name, _TRUNCATE);
    swprintf_s(g_model_tip, 512, L"%s\r\n%d 节点 · %d 杆件 · %d 自由度%s",
               name, g_model->n, g_model->m, g_model->free_dof, g_preset == 0 ?
               L"\r\n初始 l = h = 1 m 为示例参数。\r\n几何条件：h > l / (2√3)" : L"");
    /* 预设名称只在选择框出现；导入文件另显示短名，完整名称放在悬停提示。 */
    if (wcslen(name) > 18) { name[17] = L'…'; name[18] = 0; }
    if (g_preset == 3)
        swprintf_s(info, 512, L"%s\r\n%d 节点 · %d 杆件 · %d 自由度",
                   name, g_model->n, g_model->m, g_model->free_dof);
    else swprintf_s(info, 512, L"%d 节点 · %d 杆件 · %d 自由度",
                   g_model->n, g_model->m, g_model->free_dof);
    set_text(IDC_MODEL_INFO, info);
    if (g_tooltip) {
        const int targets[] = {IDC_MODEL_INFO, IDC_PRESET};
        int target;
        TOOLINFOW tip; ZeroMemory(&tip, sizeof(tip)); tip.cbSize = sizeof(tip);
        tip.hwnd = g_window;
        tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.lpszText = g_model_tip;
        for (target = 0; target < 2; ++target) {
            tip.uId = (UINT_PTR)ctl(targets[target]);
            SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tip);
        }
    }
    for (i = 1; i < g_model->m; ++i) {
        if (g_model->elements[i].E != g_model->elements[0].E) uniform_e = FALSE;
        if (g_model->elements[i].A != g_model->elements[0].A) uniform_a = FALSE;
    }
    if (uniform_e) {
        if (g_model->elements[0].E == 2e7) wcscpy_s(material, 96, L"2 × 10⁷ kN/m²");
        else swprintf_s(material, 96, L"%.6g kN/m²", g_model->elements[0].E);
    } else wcscpy_s(material, 96, L"多种材料");
    set_text(IDC_E_VALUE, material);
    if (uniform_a) swprintf_s(material, 96, L"%.6g m²", g_model->elements[0].A);
    else wcscpy_s(material, 96, L"多个截面");
    set_text(IDC_A_VALUE, material);
}

static void update_controls(void)
{
    const int buttons[] = {IDC_PRESET, IDC_OPEN_MODEL, IDC_SAVE_MODEL,
                           IDC_SAVE_RESULTS, IDC_CALCULATE};
    size_t i;
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
        EnableWindow(ctl(buttons[i]), !g_busy);
    EnableWindow(ctl(IDC_LENGTH), !g_busy && g_preset == 0);
    EnableWindow(ctl(IDC_HEIGHT), !g_busy && g_preset == 0);
    ShowWindow(ctl(IDC_LENGTH), g_preset == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(ctl(IDC_HEIGHT), g_preset == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(ctl(IDC_LENGTH_LABEL), g_preset == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(ctl(IDC_HEIGHT_LABEL), g_preset == 0 ? SW_SHOW : SW_HIDE);
    EnableWindow(ctl(IDC_CALCULATE), !g_busy && g_input_valid && g_model != NULL);
    EnableWindow(ctl(IDC_SAVE_MODEL), !g_busy && g_input_valid && g_model != NULL);
    EnableWindow(ctl(IDC_SAVE_RESULTS), !g_busy && g_input_valid && g_model && g_model->solved);
    SetWindowTextW(ctl(IDC_CALCULATE), g_busy ? L"正在计算…" : L"计算    F5");
    refresh_model_info();
    layout();
    InvalidateRect(ctl(IDC_CALCULATE), NULL, TRUE);
}

/* 对话框只把路径写进临时 buffer；取消不会改变当前模型或结果。 */
static BOOL choose_file(BOOL save, BOOL results, wchar_t *path, size_t cap)
{
    OPENFILENAMEW dialog;
    wchar_t initial[MAX_PATH];
    ZeroMemory(&dialog, sizeof(dialog));
#ifdef TRUSS_STANDALONE
    /* 单文件内部运行目录会在退出时清理；用户文件默认保存在 EXE 所在目录。 */
    wcsncpy_s(initial, MAX_PATH, g_export_dir, _TRUNCATE);
#else
    swprintf_s(initial, MAX_PATH, L"%s\\..\\%s", g_program_dir,
               results ? L"计算结果" : L"总控信息");
#endif
    if (save) {
        const wchar_t *file = results ? L"桁架计算结果.txt" : L"桁架总控信息.txt";
        wcsncpy_s(path, cap, file, _TRUNCATE);
    } else path[0] = 0;
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"文本文档 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)cap;
    dialog.lpstrInitialDir = initial;
    dialog.lpstrDefExt = L"txt";
    dialog.lpstrTitle = save ? (results ? L"导出计算结果" : L"导出总控信息") : L"导入桁架总控信息";
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                    (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
}

static void show_error(const wchar_t *error)
{
    set_status(error);
    MessageBoxW(g_window, error, L"平面桁架 · 操作未完成", MB_OK | MB_ICONWARNING);
}

static BOOL load_model(const wchar_t *path, int preset)
{
    wchar_t error[1024];
    UIModel *model = ui_load_model(path, error, 1024);
    if (!model) { show_error(error); return FALSE; }
    ui_free_model(g_model);
    g_model = model;
    g_preset = preset;
    g_input_valid = TRUE;
    g_calculation_failed = FALSE;
    g_view.selected_element = -1;
    wcsncpy_s(g_input_path, MAX_PATH, path, _TRUNCATE);
    SendMessageW(ctl(IDC_PRESET), CB_SETCURSEL, preset, 0);
    set_text(IDC_INPUT_ERROR, L"");
    set_status(L"模型已载入");
    fit_view();
    update_controls();
    refresh_results();
    return TRUE;
}

static void import_model(void)
{
    wchar_t path[MAX_PATH];
    if (g_busy) return;
    if (choose_file(FALSE, FALSE, path, MAX_PATH)) load_model(path, 3);
    SendMessageW(ctl(IDC_PRESET), CB_SETCURSEL, g_preset, 0);
}

static void change_preset(void)
{
    int selected = (int)SendMessageW(ctl(IDC_PRESET), CB_GETCURSEL, 0, 0);
    wchar_t path[MAX_PATH];
    if (g_busy || selected == g_preset) return;
    if (selected == 0) {
        g_preset = 0;
        parameters_changed();
        fit_view();
    } else if (selected == 3) import_model();
    else {
        swprintf_s(path, MAX_PATH, L"%s\\..\\总控信息\\%s", g_program_dir,
           selected == 1 ? L"课件四杆_验证算例.txt" : L"七杆桁架_验证算例.txt");
        if (!load_model(path, selected))
            SendMessageW(ctl(IDC_PRESET), CB_SETCURSEL, g_preset, 0);
    }
}

static void export_file(BOOL results)
{
    wchar_t path[MAX_PATH], error[1024];
    BOOL success;
    if (g_busy || !g_input_valid || !g_model || (results && !g_model->solved)) return;
    if (!choose_file(TRUE, results, path, MAX_PATH)) return;
    success = results ? ui_save_results(g_model, path, error, 1024) :
                        ui_save_model(g_model, path, error, 1024);
    if (success) set_status(results ? L"计算结果已导出。" : L"总控信息已导出。");
    else show_error(error);
}

/* 后台线程不读写控件，也不碰当前模型。关闭窗口会等待其自然完成，
 * 确保 truss.exe 进程以及模型内存不会因退出界面被中途遗弃。 */
static DWORD WINAPI solve_worker(LPVOID parameter)
{
    SolveTask *task = (SolveTask *)parameter;
    task->success = ui_solve_model(task->model, task->solver, task->error, 1024);
    PostMessageW(task->owner, WM_SOLVE_DONE, 0, 0);
    return 0;
}

static void calculate(void)
{
    if (g_busy || !g_input_valid || !g_model) return;
    g_task = (SolveTask *)calloc(1, sizeof(SolveTask));
    if (!g_task) { show_error(L"内存不足，无法启动计算。请关闭其他程序后重试。"); return; }
    g_task->owner = g_window;
    g_task->model = ui_clone_model(g_model);
    if (!g_task->model) {
        free(g_task); g_task = NULL;
        show_error(L"内存不足，无法复制计算模型。"); return;
    }
    swprintf_s(g_task->solver, MAX_PATH, L"%s\\truss.exe", g_program_dir);
    discard_results(g_model);
    discard_results(g_task->model);
    g_calculation_failed = FALSE;
    set_text(IDC_INPUT_ERROR, L"");
    g_busy = TRUE;
    g_thread = CreateThread(NULL, 0, solve_worker, g_task, 0, NULL);
    if (!g_thread) {
        g_busy = FALSE;
        ui_free_model(g_task->model); free(g_task); g_task = NULL;
        show_error(L"无法启动计算线程。请稍后重试。");
    } else set_status(L"正在计算…");
    update_controls();
    refresh_results();
    InvalidateRect(g_canvas, NULL, FALSE);
}

static void calculation_done(void)
{
    SolveTask *task = g_task;
    if (!task) return;
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread); g_thread = NULL;
    g_task = NULL; g_busy = FALSE;
    if (task->success) {
        ui_free_model(g_model);
        g_model = task->model;
        task->model = NULL;
        set_status(L"计算完成");
    } else {
        g_calculation_failed = TRUE;
        set_status(task->error[0] ? task->error : L"计算失败。请检查约束、几何与总控信息。");
        set_text(IDC_INPUT_ERROR, task->error);
    }
    ui_free_model(task->model); free(task);
    update_controls(); refresh_results();
    InvalidateRect(g_canvas, NULL, FALSE);
    if (g_close_pending) DestroyWindow(g_window);
}

/* 结果表按当前页重建列，计算值始终来自同一份已完成模型。 */
static void table_column(int index, const wchar_t *title, int width)
{
    LVCOLUMNW column;
    ZeroMemory(&column, sizeof(column));
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = (LPWSTR)title;
    column.cx = px(width);
    column.fmt = index == 0 ? LVCFMT_LEFT : LVCFMT_RIGHT;
    ListView_InsertColumn(g_table, index, &column);
}
static void table_cell(int row, int col, const wchar_t *text)
{
    if (col == 0) {
        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = row;
        item.pszText = (LPWSTR)text; item.lParam = row;
        ListView_InsertItem(g_table, &item);
    } else ListView_SetItemText(g_table, row, col, (LPWSTR)text);
}
static void number_cell(int row, int col, double number)
{
    wchar_t text[80];
    swprintf_s(text, 80, L"%.6g", number == 0.0 ? 0.0 : number);
    table_cell(row, col, text);
}
static void integer_cell(int row, int col, int number)
{
    wchar_t text[32]; swprintf_s(text, 32, L"%d", number); table_cell(row, col, text);
}
static void check_row(int row, const wchar_t *name, double value, const wchar_t *unit)
{
    table_cell(row, 0, name); number_cell(row, 1, value); table_cell(row, 2, unit);
}

static void refresh_results(void)
{
    int i;
    BOOL solved = g_input_valid && g_model && g_model->solved && !g_busy;
    wchar_t summary[256];
    SendMessageW(g_table, WM_SETREDRAW, FALSE, 0);
    g_selecting = TRUE;
    ListView_DeleteAllItems(g_table);
    while (ListView_DeleteColumn(g_table, 0)) { }
    if (solved) {
        swprintf_s(summary, 256, L"最大位移 %.5g mm    ·    最大 |N| %.5g kN",
                   g_model->max_displacement * 1000.0, g_model->max_axial);
        set_text(IDC_RESULT_SUMMARY, summary);
        if (g_active_tab == 0) {
            table_column(0, L"节点", 70); table_column(1, L"ux / mm", 165);
            table_column(2, L"uy / mm", 165); table_column(3, L"位移模 / mm", 165);
            table_column(4, L"约束状态", 130);
            for (i = 0; i < g_model->n; ++i) {
                UINode *n = &g_model->nodes[i];
                integer_cell(i, 0, n->id); number_cell(i, 1, n->ux * 1000.0);
                number_cell(i, 2, n->uy * 1000.0);
                number_cell(i, 3, hypot(n->ux, n->uy) * 1000.0);
                table_cell(i, 4, n->fix_x && n->fix_y ? L"x、y 约束" :
                      n->fix_x ? L"x 约束" : n->fix_y ? L"y 约束" : L"自由节点");
            }
        } else if (g_active_tab == 1) {
            table_column(0, L"杆件", 65); table_column(1, L"起点 → 终点", 115);
            table_column(2, L"杆长 / m", 115); table_column(3, L"N / kN", 155);
            table_column(4, L"受力", 65); table_column(5, L"伸长 / mm", 130);
            table_column(6, L"σ / kN/m²", 150);
            for (i = 0; i < g_model->m; ++i) {
                UIElement *e = &g_model->elements[i]; wchar_t pair[64];
                integer_cell(i, 0, e->id);
                swprintf_s(pair, 64, L"%d → %d", g_model->nodes[e->ni].id, g_model->nodes[e->nj].id);
                table_cell(i, 1, pair); number_cell(i, 2, e->length); number_cell(i, 3, e->axial);
                table_cell(i, 4, fabs(e->axial) < 1e-9 ? L"零力" : e->axial > 0 ? L"受拉" : L"受压");
                number_cell(i, 5, e->extension * 1000.0); number_cell(i, 6, e->stress);
            }
            if (g_view.selected_element >= 0 && g_view.selected_element < g_model->m)
                ListView_SetItemState(g_table, g_view.selected_element,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        } else if (g_active_tab == 2) {
            int row = 0;
            table_column(0, L"支座节点", 100); table_column(1, L"Rx / kN", 175);
            table_column(2, L"Ry / kN", 175); table_column(3, L"约束方向", 155);
            for (i = 0; i < g_model->n; ++i) {
                UINode *n = &g_model->nodes[i];
                if (!n->fix_x && !n->fix_y) continue;
                integer_cell(row, 0, n->id);
                if (n->fix_x) number_cell(row, 1, n->rx); else table_cell(row, 1, L"—");
                if (n->fix_y) number_cell(row, 2, n->ry); else table_cell(row, 2, L"—");
                table_cell(row++, 3, n->fix_x && n->fix_y ? L"x、y" : n->fix_x ? L"x" : L"y");
            }
        } else if (g_active_tab == 3) {
            table_column(0, L"校核项目", 260); table_column(1, L"计算值", 230);
            table_column(2, L"单位 / 说明", 240);
            check_row(0, L"自由位移分量", (double)g_model->free_dof, L"个");
            check_row(1, L"自由节点残差最大值", g_model->residual, L"kN");
            check_row(2, L"相对平衡残差", g_model->relative_residual, L"无量纲");
            check_row(3, L"整体 x 向平衡", g_model->balance[0], L"kN");
            check_row(4, L"整体 y 向平衡", g_model->balance[1], L"kN");
            check_row(5, L"整体力矩平衡", g_model->balance[2], L"kN·m");
            check_row(6, L"应变能 U", g_model->energy, L"kN·m");
            check_row(7, L"力与位移内积 W", g_model->work, L"kN·m");
            check_row(8, L"能量恒等式误差 2U − W", g_model->energy_error, L"kN·m");
        }
        if (g_model->raw_output) {
            int count = MultiByteToWideChar(CP_UTF8, 0, g_model->raw_output, -1, NULL, 0);
            wchar_t *raw = count > 0 ? (wchar_t *)calloc((size_t)count, sizeof(wchar_t)) : NULL;
            if (raw) {
                MultiByteToWideChar(CP_UTF8, 0, g_model->raw_output, -1, raw, count);
                SetWindowTextW(g_raw, raw); free(raw);
            }
        }
    } else {
        set_text(IDC_RESULT_SUMMARY, L"");
        SetWindowTextW(g_raw, L"");
        SetWindowTextW(g_empty, g_busy ? L"正在计算，请稍候…" : !g_input_valid ?
               L"请先修正输入参数" : g_calculation_failed ?
               L"计算未完成 · 请检查模型" : L"计算后查看结果");
    }
    g_selecting = FALSE;
    SendMessageW(g_table, WM_SETREDRAW, TRUE, 0);
    ShowWindow(g_table, solved && g_active_tab != 4 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_raw, solved && g_active_tab == 4 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_empty, solved ? SW_HIDE : SW_SHOW);
    size_table_columns();
    InvalidateRect(g_tab, NULL, FALSE);
    InvalidateRect(g_table, NULL, TRUE);
}

/* 列按可见空间分配，较小窗口也能直接读完结果。 */
static void size_table_columns(void)
{
    static const int weights[4][7] = {
        {60, 115, 115, 130, 100, 0, 0},
        {52, 90, 90, 115, 60, 110, 125},
        {90, 145, 145, 110, 0, 0, 0},
        {230, 150, 180, 0, 0, 0, 0}
    };
    HWND header; RECT rect; int count, i, total = 0, available, used = 0;
    if (!g_table || g_active_tab == 4) return;
    header = ListView_GetHeader(g_table);
    count = Header_GetItemCount(header);
    if (count < 1) return;
    GetClientRect(g_table, &rect);
    available = max(1, rect.right - px(2));
    for (i = 0; i < count; ++i) total += weights[g_active_tab][i];
    for (i = 0; i < count; ++i) {
        int width = i == count - 1 ? available - used :
                    MulDiv(available, weights[g_active_tab][i], total);
        ListView_SetColumnWidth(g_table, i, max(1, width)); used += width;
    }
}

static void layout(void)
{
    RECT client; int w, h, margin = px(20), gap = px(18);
    int leftw = px(264), rightx, rightw, top = px(82), bottom;
    int drawtop = top + px(42), drawh, resulty, tabley, tableh;
    int pad = px(20), fieldw, labely, edity, material_y, error_y, error_h, tabsw;
    GetClientRect(g_window, &client); w = client.right; h = client.bottom;
    /* 特别窄的显示器仍给画布与结果留足宽度。 */
    if (w < px(1050)) leftw = px(250);
    rightx = margin + leftw + gap; rightw = w - rightx - margin;
    bottom = h - px(38);
    /* 常用窗口为六杆结果预留完整六行；小窗口保留画布并允许结果滚动。 */
    drawh = bottom - drawtop - gap - px(336);
    drawh = max(px(180), drawh);
    resulty = drawtop + drawh + gap;
    g_left_panel = (RECT){margin, top, margin + leftw, bottom};
    g_result_panel = (RECT){rightx, resulty, rightx + rightw, bottom};
    move_control(IDC_TITLE, margin + px(50), px(22), px(310), px(34));
    move_control(IDC_OPEN_MODEL, w - margin - px(314), px(22), px(98), px(36));
    move_control(IDC_SAVE_MODEL, w - margin - px(208), px(22), px(98), px(36));
    move_control(IDC_SAVE_RESULTS, w - margin - px(102), px(22), px(102), px(36));
    move_control(IDC_MODEL_TITLE, margin + pad, top + px(22), leftw - pad * 2, px(26));
    move_control(IDC_PRESET, margin + pad, top + px(62), leftw - pad * 2, px(220));
    move_control(IDC_MODEL_INFO, margin + pad, top + px(108), leftw - pad * 2,
                 px(g_preset == 3 ? 40 : 22));
    labely = top + px(152); edity = labely + px(28);
    fieldw = (leftw - pad * 2 - px(14)) / 2;
    move_control(IDC_LENGTH_LABEL, margin + pad, labely, fieldw, px(22));
    move_control(IDC_HEIGHT_LABEL, margin + pad + fieldw + px(14), labely, fieldw, px(22));
    move_control(IDC_LENGTH, margin + pad, edity, fieldw, px(40));
    move_control(IDC_HEIGHT, margin + pad + fieldw + px(14), edity, fieldw, px(40));
    SetWindowRgn(ctl(IDC_LENGTH), CreateRoundRectRgn(0, 0, fieldw + 1, px(40) + 1, px(8), px(8)), TRUE);
    SetWindowRgn(ctl(IDC_HEIGHT), CreateRoundRectRgn(0, 0, fieldw + 1, px(40) + 1, px(8), px(8)), TRUE);
    /* 侧栏随实际字段收紧，说明文字不占位；错误区和计算按钮保持独立。 */
    g_model_divider_y = top + px(g_preset == 0 ? 244 : g_preset == 3 ? 166 : 146);
    material_y = g_model_divider_y + px(22);
    move_control(IDC_E_LABEL, margin + pad, material_y, leftw - pad * 2, px(22));
    move_control(IDC_E_VALUE, margin + pad, material_y + px(29), leftw - pad * 2, px(24));
    move_control(IDC_A_LABEL, margin + pad, material_y + px(70), leftw - pad * 2, px(22));
    move_control(IDC_A_VALUE, margin + pad, material_y + px(99), leftw - pad * 2, px(24));
    error_y = max(material_y + px(143), bottom - px(137));
    error_h = max(px(42), bottom - px(79) - error_y);
    move_control(IDC_INPUT_ERROR, margin + pad, error_y, leftw - pad * 2, error_h);
    move_control(IDC_CALCULATE, margin + pad, bottom - px(64), leftw - pad * 2, px(44));
    move_control(IDC_STRUCTURE_TITLE, rightx + px(2), top + px(6), px(135), px(25));
    g_view_group = (RECT){rightx + rightw - px(304), top, rightx + rightw - px(96), top + px(34)};
    move_control(IDC_VIEW_GEOMETRY, g_view_group.left + px(3), top + px(3), px(66), px(28));
    move_control(IDC_VIEW_AXIAL, g_view_group.left + px(71), top + px(3), px(66), px(28));
    move_control(IDC_VIEW_DEFORMED, g_view_group.left + px(139), top + px(3), px(66), px(28));
    move_control(IDC_FIT_VIEW, rightx + rightw - px(86), top, px(86), px(34));
    move_control(IDC_CANVAS, rightx, drawtop, rightw, drawh);
    SetWindowRgn(g_canvas, CreateRoundRectRgn(0, 0, rightw + 1, drawh + 1, px(12), px(12)), TRUE);
    move_control(IDC_RESULT_TITLE, rightx + px(18), resulty + px(16), px(64), px(25));
    move_control(IDC_RESULT_SUMMARY, rightx + px(88), resulty + px(19), rightw - px(106), px(22));
    tabsw = min(px(510), rightw - px(36));
    move_control(IDC_RESULT_TAB, rightx + px(18), resulty + px(49), tabsw, px(37));
    TabCtrl_SetItemSize(g_tab, (tabsw - px(12)) / 5, px(33));
    tabley = resulty + px(94); tableh = max(px(70), bottom - tabley - px(16));
    move_control(IDC_RESULT_TABLE, rightx + px(18), tabley, rightw - px(36), tableh);
    move_control(IDC_RAW_OUTPUT, rightx + px(18), tabley, rightw - px(36), tableh);
    move_control(IDC_RESULT_EMPTY, rightx + px(18), tabley + max(0, (tableh - px(26)) / 2),
                 rightw - px(36), px(26));
    move_control(IDC_STATUS_TEXT, margin + px(16), h - px(27), w - margin * 2 - px(16), px(22));
    style_control_metrics(); size_table_columns();
    InvalidateRect(g_window, NULL, TRUE);
}

/* 表面与按钮使用相同的克制色板；保留原生控件焦点框和键盘导航。 */
static void draw_button(const DRAWITEMSTRUCT *item)
{
    wchar_t text[128]; RECT rect = item->rcItem;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL primary = item->CtlID == IDC_CALCULATE;
    BOOL segment = item->CtlID >= IDC_VIEW_GEOMETRY && item->CtlID <= IDC_VIEW_DEFORMED;
    BOOL selected = segment && (int)item->CtlID - IDC_VIEW_GEOMETRY == g_view.mode;
    BOOL hover = GetPropW(item->hwndItem, L"TrussHover") != NULL;
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    COLORREF bg = primary ? (disabled ? RGB(44,66,92) : pressed ? RGB(12,75,165) :
                   hover ? RGB(25,112,225) : UI_ACCENT) :
                   disabled ? UI_SOFT : selected ? UI_SURFACE :
                   pressed ? RGB(220,231,242) : hover ? RGB(232,241,251) :
                   segment ? UI_LINE : UI_SURFACE;
    COLORREF border = primary ? bg : selected ? UI_LINE : segment ? bg :
                      hover || pressed ? RGB(172,200,230) : UI_LINE;
    HBRUSH backdrop = CreateSolidBrush(primary ? UI_PANEL : segment ? UI_LINE : UI_BG);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(item->hDC, &rect, backdrop); DeleteObject(backdrop);
    HGDIOBJ oldpen = SelectObject(item->hDC, pen), oldbrush = SelectObject(item->hDC, brush);
    RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, px(10), px(10));
    SelectObject(item->hDC, oldpen); SelectObject(item->hDC, oldbrush);
    DeleteObject(pen); DeleteObject(brush);
    GetWindowTextW(item->hwndItem, text, 128);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? (primary ? UI_PANEL_MUTED : RGB(153,167,180)) :
                 primary ? UI_SURFACE : selected ? UI_ACCENT : UI_INK);
    SelectObject(item->hDC, primary ? g_section_font : g_font);
    if (item->itemState & ODS_SELECTED) OffsetRect(&rect, 1, 1);
    DrawTextW(item->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (item->itemState & ODS_FOCUS) {
        InflateRect(&rect, -px(5), -px(5)); DrawFocusRect(item->hDC, &rect);
    }
}

static LRESULT CALLBACK button_subclass(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (message == WM_MOUSEMOVE && !GetPropW(window, L"TrussHover")) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
        SetPropW(window, L"TrussHover", (HANDLE)(INT_PTR)1);
        TrackMouseEvent(&track); InvalidateRect(window, NULL, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(window, L"TrussHover"); InvalidateRect(window, NULL, FALSE);
    } else if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, NULL, FALSE);
    } else if (message == WM_NCDESTROY) RemovePropW(window, L"TrussHover");
    return DefSubclassProc(window, message, wparam, lparam);
}

static void paint_round_rect(HDC dc, const RECT *rect, COLORREF fill, COLORREF border, int radius)
{
    HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldbrush = SelectObject(dc, brush), oldpen = SelectObject(dc, pen);
    RoundRect(dc, rect->left, rect->top, rect->right, rect->bottom, px(radius), px(radius));
    SelectObject(dc, oldbrush); SelectObject(dc, oldpen); DeleteObject(brush); DeleteObject(pen);
}

/* 标签仍是原生 Tab 控件，鼠标、箭头键和辅助技术保留原有行为。 */
static LRESULT CALLBACK tab_subclass(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint; RECT rect; HDC dc = BeginPaint(window, &paint);
        int i, current = TabCtrl_GetCurSel(window), count = TabCtrl_GetItemCount(window);
        GetClientRect(window, &rect); FillRect(dc, &rect, g_surface_brush);
        SetBkMode(dc, TRANSPARENT); SelectObject(dc, g_font);
        for (i = 0; i < count; ++i) {
            RECT item; wchar_t text[64]; TCITEMW tab;
            ZeroMemory(&tab, sizeof(tab)); tab.mask = TCIF_TEXT;
            tab.pszText = text; tab.cchTextMax = 64;
            TabCtrl_GetItem(window, i, &tab); TabCtrl_GetItemRect(window, i, &item);
            item.top = px(1); item.bottom = rect.bottom - px(3);
            if (i == current) paint_round_rect(dc, &item, RGB(233,242,253), RGB(233,242,253), 9);
            SetTextColor(dc, i == current ? UI_ACCENT : UI_MUTED);
            DrawTextW(dc, text, -1, &item, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (GetFocus() == window && i == TabCtrl_GetCurFocus(window)) {
                InflateRect(&item, -px(4), -px(4)); DrawFocusRect(dc, &item);
            }
        }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_KEYDOWN ||
        message == WM_LBUTTONUP || message == TCM_SETCURSEL || message == TCM_SETCURFOCUS) {
        LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        InvalidateRect(window, NULL, FALSE); return result;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static LRESULT CALLBACK header_subclass(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (message == HDM_LAYOUT) {
        HDLAYOUT *layout = (HDLAYOUT *)lparam;
        LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        layout->pwpos->cy = px(33); layout->prc->top = px(33);
        return result;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint; RECT rect; HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(UI_SOFT); int i, count = Header_GetItemCount(window);
        GetClientRect(window, &rect); FillRect(dc, &rect, brush); DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT); SelectObject(dc, g_small_font); SetTextColor(dc, UI_MUTED);
        for (i = 0; i < count; ++i) {
            HDITEMW item; RECT cell; wchar_t text[96];
            ZeroMemory(&item, sizeof(item)); item.mask = HDI_TEXT | HDI_FORMAT;
            item.pszText = text; item.cchTextMax = 96;
            Header_GetItem(window, i, &item); Header_GetItemRect(window, i, &cell);
            cell.left += px(10); cell.right -= px(10);
            DrawTextW(dc, text, -1, &cell, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      (item.fmt & HDF_RIGHT ? DT_RIGHT : DT_LEFT));
        }
        EndPaint(window, &paint); return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static LRESULT CALLBACK input_subclass(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    /* 多行 Edit 的 WM_SETTEXT 不发 EN_CHANGE；程序设值仍须立即校验。 */
    if (message == WM_SETTEXT) {
        LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        if (result) SendMessageW(g_window, WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(window), EN_CHANGE), (LPARAM)window);
        return result;
    }
    if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\n')) return 0;
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        RECT rect; GetWindowRect(window, &rect);
        MapWindowPoints(NULL, g_window, (POINT *)&rect, 2); InflateRect(&rect, px(2), px(2));
        InvalidateRect(g_window, &rect, FALSE);
    }
    if (message == WM_KEYDOWN && wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(window, EM_SETSEL, 0, -1); return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static void style_control_metrics(void)
{
    const int inputs[] = {IDC_LENGTH, IDC_HEIGHT}; int i; RECT rect;
    for (i = 0; i < 2; ++i) {
        GetClientRect(ctl(inputs[i]), &rect);
        rect.left = px(12); rect.right -= px(10); rect.top = px(10); rect.bottom -= px(4);
        SendMessageW(ctl(inputs[i]), EM_SETRECTNP, 0, (LPARAM)&rect);
    }
    GetClientRect(g_raw, &rect); InflateRect(&rect, -px(10), -px(8));
    SendMessageW(g_raw, EM_SETRECTNP, 0, (LPARAM)&rect);
    SendMessageW(ctl(IDC_PRESET), CB_SETITEMHEIGHT, (WPARAM)-1, px(29));
    SendMessageW(ctl(IDC_PRESET), CB_SETITEMHEIGHT, 0, px(31));
}

static void set_table_row_height(void)
{
    HIMAGELIST previous = g_row_images;
    g_row_images = ImageList_Create(1, px(32), ILC_COLOR32, 1, 1);
    ListView_SetImageList(g_table, g_row_images, LVSIL_SMALL);
    if (previous) ImageList_Destroy(previous);
}

static LRESULT draw_table_cell(NMLVCUSTOMDRAW *draw)
{
    int row = (int)draw->nmcd.dwItemSpec, col = draw->iSubItem;
    BOOL selected = (ListView_GetItemState(g_table, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    BOOL words = (g_active_tab == 0 && col == 4) ||
                 (g_active_tab == 1 && col == 4) ||
                 (g_active_tab == 2 && col == 3) ||
                 (g_active_tab == 3 && (col == 0 || col == 2));
    RECT rect; wchar_t text[256]; HBRUSH brush;
    COLORREF color = UI_INK, fill = selected ? RGB(230,241,254) : row % 2 ? UI_SOFT : UI_SURFACE;
    ListView_GetSubItemRect(g_table, row, col, LVIR_BOUNDS, &rect);
    if (col == 0) rect.right = rect.left + ListView_GetColumnWidth(g_table, 0);
    brush = CreateSolidBrush(fill); FillRect(draw->nmcd.hdc, &rect, brush); DeleteObject(brush);
    if (g_active_tab == 1 && g_model && row >= 0 && row < g_model->m && col >= 3) {
        double axial = g_model->elements[row].axial;
        color = fabs(axial) < 1e-9 ? UI_MUTED : axial > 0 ? UI_TENSION_INK : UI_COMPRESSION_INK;
    }
    ListView_GetItemText(g_table, row, col, text, 256);
    rect.left += px(10); rect.right -= px(10);
    SetBkMode(draw->nmcd.hdc, TRANSPARENT); SetTextColor(draw->nmcd.hdc, color);
    SelectObject(draw->nmcd.hdc, words ? g_font : g_number_font);
    DrawTextW(draw->nmcd.hdc, text, -1, &rect, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
              (col == 0 ? DT_LEFT : DT_RIGHT));
    return CDRF_SKIPDEFAULT;
}

static void draw_brand_mark(HDC dc, int x, int y, int size)
{
    RECT rect = {x, y, x + size, y + size};
    HBRUSH brush = CreateSolidBrush(UI_ACCENT), dot = CreateSolidBrush(UI_SURFACE);
    HPEN border = CreatePen(PS_SOLID, 1, UI_ACCENT);
    HPEN pen = CreatePen(PS_SOLID, max(1, size / 20), UI_SURFACE);
    HGDIOBJ oldbrush = SelectObject(dc, brush), oldpen = SelectObject(dc, border);
    int ax = x + size * 18 / 100, bx = x + size / 2, cx = x + size * 82 / 100;
    int ty = y + size * 24 / 100, by = y + size * 76 / 100, r = max(1, size * 4 / 100);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, size / 4, size / 4);
    SelectObject(dc, pen);
    MoveToEx(dc, ax, by, NULL); LineTo(dc, bx, ty); LineTo(dc, cx, by); LineTo(dc, ax, by);
    MoveToEx(dc, bx, ty, NULL); LineTo(dc, bx, by);
    SelectObject(dc, dot);
    Ellipse(dc, ax - r, by - r, ax + r + 1, by + r + 1);
    Ellipse(dc, bx - r, ty - r, bx + r + 1, ty + r + 1);
    Ellipse(dc, cx - r, by - r, cx + r + 1, by + r + 1);
    Ellipse(dc, bx - r, by - r, bx + r + 1, by + r + 1);
    SelectObject(dc, oldbrush); SelectObject(dc, oldpen);
    DeleteObject(brush); DeleteObject(dot); DeleteObject(border); DeleteObject(pen);
}

static HICON create_app_icon(void)
{
    BITMAPINFO info; ICONINFO icon; void *pixels = NULL; HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color, mask; HGDIOBJ old; HBRUSH clear; RECT rect = {0, 0, 32, 32};
    BYTE mask_bits[128] = {0};
    HICON result; int i; unsigned long *argb;
    ZeroMemory(&info, sizeof(info)); info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 32; info.bmiHeader.biHeight = -32;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!color) { DeleteDC(dc); return NULL; }
    old = SelectObject(dc, color); clear = CreateSolidBrush(RGB(1, 2, 3));
    FillRect(dc, &rect, clear); DeleteObject(clear); draw_brand_mark(dc, 0, 0, 32);
    GdiFlush(); argb = (unsigned long *)pixels;
    for (i = 0; i < 32 * 32; ++i)
        argb[i] = (argb[i] & 0x00ffffffUL) == 0x00010203UL ? 0 : argb[i] | 0xff000000UL;
    SelectObject(dc, old); DeleteDC(dc);
    mask = CreateBitmap(32, 32, 1, 1, mask_bits);
    ZeroMemory(&icon, sizeof(icon)); icon.fIcon = TRUE; icon.hbmColor = color; icon.hbmMask = mask;
    result = CreateIconIndirect(&icon); DeleteObject(color); DeleteObject(mask); return result;
}

static LRESULT CALLBACK raw_subclass(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (message == WM_KEYDOWN && wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(window, EM_SETSEL, 0, -1); return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static LRESULT CALLBACK canvas_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint; RECT rect; HDC dc, buffer; HBITMAP bitmap; HGDIOBJ old;
        dc = BeginPaint(window, &paint); GetClientRect(window, &rect);
        buffer = CreateCompatibleDC(dc);
        bitmap = CreateCompatibleBitmap(dc, max(1, rect.right), max(1, rect.bottom));
        old = SelectObject(buffer, bitmap);
        ui_draw_structure(buffer, &rect, g_model, &g_view, g_dpi);
        if (!g_input_valid) {
            RECT note = {px(16), px(12), rect.right - px(16), px(42)};
            SetBkMode(buffer, TRANSPARENT); SelectObject(buffer, g_small_font);
            SetTextColor(buffer, UI_COMPRESSION);
            DrawTextW(buffer, L"参数输入无效 · 此图保留最近有效几何", -1, &note, DT_LEFT | DT_SINGLELINE);
        }
        BitBlt(dc, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer);
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        double factor = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1.12 : 1.0 / 1.12;
        g_view.zoom *= factor;
        if (g_view.zoom < 0.35) g_view.zoom = 0.35;
        if (g_view.zoom > 5.0) g_view.zoom = 5.0;
        InvalidateRect(window, NULL, FALSE); return 0;
    }
    if (message == WM_LBUTTONDOWN && g_model) {
        RECT rect; int selected;
        GetClientRect(window, &rect);
        selected = ui_hit_element(&rect, g_model, &g_view,
                                 GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        g_view.selected_element = selected;
        if (selected >= 0 && g_model->solved && g_input_valid && !g_busy) {
            g_active_tab = 1; TabCtrl_SetCurSel(g_tab, 1); refresh_results();
            ListView_EnsureVisible(g_table, selected, FALSE);
        }
        InvalidateRect(window, NULL, FALSE); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void create_controls(void)
{
    const wchar_t *tabs[] = {L"位移", L"杆件轴力", L"支反力", L"计算校核", L"原始输出"};
    int i;
    make_label(IDC_TITLE, L"平面桁架");
    make_button(IDC_OPEN_MODEL, L"导入模型");
    make_button(IDC_SAVE_MODEL, L"导出总控");
    make_button(IDC_SAVE_RESULTS, L"导出结果");
    make_label(IDC_MODEL_TITLE, L"模型");
    make_control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, IDC_PRESET);
    SendMessageW(ctl(IDC_PRESET), CB_ADDSTRING, 0, (LPARAM)L"六杆桁架 · 示例");
    SendMessageW(ctl(IDC_PRESET), CB_ADDSTRING, 0, (LPARAM)L"课件四杆验证");
    SendMessageW(ctl(IDC_PRESET), CB_ADDSTRING, 0, (LPARAM)L"七杆桁架验证");
    SendMessageW(ctl(IDC_PRESET), CB_ADDSTRING, 0, (LPARAM)L"导入模型");
    SendMessageW(ctl(IDC_PRESET), CB_SETCURSEL, 0, 0);
    make_label(IDC_MODEL_INFO, L"");
    g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                   CW_USEDEFAULT, CW_USEDEFAULT, g_window, NULL, g_instance, NULL);
    if (g_tooltip) {
        const int targets[] = {IDC_MODEL_INFO, IDC_PRESET};
        int target;
        TOOLINFOW tip; ZeroMemory(&tip, sizeof(tip)); tip.cbSize = sizeof(tip);
        tip.hwnd = g_window;
        tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.lpszText = g_model_tip;
        for (target = 0; target < 2; ++target) {
            tip.uId = (UINT_PTR)ctl(targets[target]);
            SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tip);
        }
        SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, px(360));
    }
    make_label(IDC_LENGTH_LABEL, L"底杆长 l / m"); make_label(IDC_HEIGHT_LABEL, L"顶高 h / m");
    make_control(L"EDIT", L"1", ES_AUTOHSCROLL | ES_MULTILINE | WS_TABSTOP, 0, IDC_LENGTH);
    make_control(L"EDIT", L"1", ES_AUTOHSCROLL | ES_MULTILINE | WS_TABSTOP, 0, IDC_HEIGHT);
    SetWindowSubclass(ctl(IDC_LENGTH), input_subclass, 1, 0);
    SetWindowSubclass(ctl(IDC_HEIGHT), input_subclass, 1, 0);
    if (g_tooltip) {
        const int inputs[] = {IDC_LENGTH, IDC_HEIGHT};
        TOOLINFOW tip; ZeroMemory(&tip, sizeof(tip)); tip.cbSize = sizeof(tip);
        tip.hwnd = g_window; tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tip.lpszText = L"长度单位 m。\r\n初始 l = h = 1 为示例参数。\r\n几何条件：h > l / (2√3)";
        for (i = 0; i < 2; ++i) {
            tip.uId = (UINT_PTR)ctl(inputs[i]);
            SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tip);
        }
    }
    make_label(IDC_E_LABEL, L"弹性模量 E"); make_label(IDC_A_LABEL, L"截面积 A");
    make_label(IDC_E_VALUE, L""); make_label(IDC_A_VALUE, L"");
    make_label(IDC_INPUT_ERROR, L"");
    make_button(IDC_CALCULATE, L"计算    F5");
    make_label(IDC_STRUCTURE_TITLE, L"结构");
    make_button(IDC_VIEW_GEOMETRY, L"几何"); make_button(IDC_VIEW_AXIAL, L"轴力");
    make_button(IDC_VIEW_DEFORMED, L"变形"); make_button(IDC_FIT_VIEW, L"适应视图");
    g_canvas = make_control(L"TrussCanvas", L"", 0, 0, IDC_CANVAS);
    make_label(IDC_RESULT_TITLE, L"结果");
    make_control(L"STATIC", L"", SS_RIGHT | SS_ENDELLIPSIS, 0, IDC_RESULT_SUMMARY);
    g_tab = make_control(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_FIXEDWIDTH, 0, IDC_RESULT_TAB);
    for (i = 0; i < 5; ++i) {
        TCITEMW tab; ZeroMemory(&tab, sizeof(tab));
        tab.mask = TCIF_TEXT; tab.pszText = (LPWSTR)tabs[i]; TabCtrl_InsertItem(g_tab, i, &tab);
    }
    SetWindowSubclass(g_tab, tab_subclass, 1, 0);
    g_table = make_control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL |
                  LVS_SHOWSELALWAYS | WS_TABSTOP, 0, IDC_RESULT_TABLE);
    ListView_SetExtendedListViewStyle(g_table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(g_table, UI_SURFACE); ListView_SetTextBkColor(g_table, UI_SURFACE);
    ListView_SetTextColor(g_table, UI_INK);
    SetWindowSubclass(ListView_GetHeader(g_table), header_subclass, 1, 0);
    set_table_row_height();
    g_raw = make_control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL |
             ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, IDC_RAW_OUTPUT);
    SendMessageW(g_raw, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    SetWindowSubclass(g_raw, raw_subclass, 1, 0);
    g_empty = make_control(L"STATIC", L"计算后查看结果", SS_CENTER, 0, IDC_RESULT_EMPTY);
    g_status = make_control(L"STATIC", L"就绪",
                            SS_LEFT | SS_ENDELLIPSIS, 0, IDC_STATUS_TEXT);
    {
        const int buttons[] = {IDC_OPEN_MODEL, IDC_SAVE_MODEL, IDC_SAVE_RESULTS, IDC_CALCULATE,
                 IDC_VIEW_GEOMETRY, IDC_VIEW_AXIAL, IDC_VIEW_DEFORMED, IDC_FIT_VIEW};
        for (i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); ++i)
            SetWindowSubclass(ctl(buttons[i]), button_subclass, 1, 0);
    }
    EnumChildWindows(g_window, apply_font, 0);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        wchar_t error[512];
        g_window = window; g_updating = TRUE;
        create_controls();
        g_model = ui_required_model(1.0, 1.0, error, 512);
        if (!g_model) { g_input_valid = FALSE; set_status(error); }
        g_updating = FALSE; update_controls(); refresh_results();
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        RECT area; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        info->ptMinTrackSize.x = min(px(1000), area.right - area.left);
        info->ptMinTrackSize.y = min(px(700), area.bottom - area.top);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lparam;
        g_dpi = HIWORD(wparam); create_fonts(); EnumChildWindows(window, apply_font, 0);
        SetWindowPos(window, NULL, suggested->left, suggested->top,
             suggested->right - suggested->left, suggested->bottom - suggested->top,
             SWP_NOZORDER | SWP_NOACTIVATE);
        set_table_row_height(); refresh_results(); layout(); return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wparam), notification = HIWORD(wparam);
        if ((id == IDC_LENGTH || id == IDC_HEIGHT) && notification == EN_CHANGE) {
            parameters_changed(); return 0;
        }
        if (id == IDC_PRESET && notification == CBN_SELCHANGE) { change_preset(); return 0; }
        if (notification != BN_CLICKED) break;
        if (id == IDC_CALCULATE) calculate();
        else if (id == IDC_OPEN_MODEL) import_model();
        else if (id == IDC_SAVE_MODEL) export_file(FALSE);
        else if (id == IDC_SAVE_RESULTS) export_file(TRUE);
        else if (id == IDC_FIT_VIEW) fit_view();
        else if (id >= IDC_VIEW_GEOMETRY && id <= IDC_VIEW_DEFORMED) {
            g_view.mode = id - IDC_VIEW_GEOMETRY;
            InvalidateRect(g_canvas, NULL, FALSE);
            InvalidateRect(ctl(IDC_VIEW_GEOMETRY), NULL, TRUE);
            InvalidateRect(ctl(IDC_VIEW_AXIAL), NULL, TRUE);
            InvalidateRect(ctl(IDC_VIEW_DEFORMED), NULL, TRUE);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *notice = (NMHDR *)lparam;
        if (notice->idFrom == IDC_RESULT_TABLE && notice->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)lparam;
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
                return draw_table_cell(draw);
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT && GetFocus() == g_table &&
                (ListView_GetItemState(g_table, (int)draw->nmcd.dwItemSpec, LVIS_SELECTED) & LVIS_SELECTED)) {
                RECT focus; ListView_GetItemRect(g_table, (int)draw->nmcd.dwItemSpec, &focus, LVIR_BOUNDS);
                InflateRect(&focus, -px(2), -px(2)); DrawFocusRect(draw->nmcd.hdc, &focus);
            }
            return CDRF_DODEFAULT;
        }
        if (notice->idFrom == IDC_RESULT_TAB && notice->code == TCN_SELCHANGE) {
            g_active_tab = TabCtrl_GetCurSel(g_tab); refresh_results(); return 0;
        }
        if (notice->idFrom == IDC_RESULT_TABLE && notice->code == LVN_ITEMCHANGED &&
             g_active_tab == 1 && !g_selecting) {
            NMLISTVIEW *change = (NMLISTVIEW *)lparam;
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED)) {
                g_view.selected_element = change->iItem;
                InvalidateRect(g_canvas, NULL, FALSE);
            }
        }
        break;
    }
    case WM_SOLVE_DONE: calculation_done(); return 0;
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT *)lparam)->CtlType == ODT_BUTTON) {
            draw_button((const DRAWITEMSTRUCT *)lparam); return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wparam; int id = GetDlgCtrlID((HWND)lparam);
        BOOL on_bg = id == IDC_TITLE || id == IDC_STRUCTURE_TITLE || id == IDC_STATUS_TEXT;
        BOOL on_panel = id >= IDC_MODEL_TITLE && id <= IDC_INPUT_ERROR;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, id == IDC_INPUT_ERROR ? RGB(255,177,126) : on_panel ?
            (id == IDC_MODEL_INFO ||
             id == IDC_E_LABEL || id == IDC_A_LABEL ? UI_PANEL_MUTED : UI_PANEL_TEXT) :
            id == IDC_RESULT_EMPTY || id == IDC_STATUS_TEXT ? UI_MUTED : UI_INK);
        SetBkColor(dc, on_bg ? UI_BG : on_panel ? UI_PANEL : UI_SURFACE);
        return (LRESULT)(on_bg ? g_bg_brush : on_panel ? g_panel_brush : g_surface_brush);
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint; RECT rect; HDC dc = BeginPaint(window, &paint);
        HPEN pen, oldpen; HBRUSH dot; HGDIOBJ oldbrush; int i;
        GetClientRect(window, &rect); FillRect(dc, &rect, g_bg_brush);
        paint_round_rect(dc, &g_left_panel, UI_PANEL, UI_PANEL, 14);
        {
            RECT shadow = g_result_panel; OffsetRect(&shadow, 0, px(2));
            paint_round_rect(dc, &shadow, RGB(226,233,240), RGB(226,233,240), 14);
        }
        paint_round_rect(dc, &g_result_panel, UI_SURFACE, UI_LINE, 14);
        paint_round_rect(dc, &g_view_group, UI_LINE, UI_LINE, 11);
        draw_brand_mark(dc, px(20), px(22), px(36));
        pen = CreatePen(PS_SOLID, 1, UI_LINE); oldpen = SelectObject(dc, pen);
        MoveToEx(dc, px(20), px(70), NULL); LineTo(dc, rect.right - px(20), px(70));
        SelectObject(dc, oldpen); DeleteObject(pen);
        pen = CreatePen(PS_SOLID, 1, UI_PANEL_LINE); oldpen = SelectObject(dc, pen);
        MoveToEx(dc, g_left_panel.left + px(20), g_model_divider_y, NULL);
        LineTo(dc, g_left_panel.right - px(20), g_model_divider_y);
        SelectObject(dc, oldpen); DeleteObject(pen);
        for (i = 0; i < 2; ++i) {
            RECT field; HWND input = ctl(i == 0 ? IDC_LENGTH : IDC_HEIGHT);
            if (!IsWindowVisible(input)) continue;
            GetWindowRect(input, &field); MapWindowPoints(NULL, window, (POINT *)&field, 2);
            InflateRect(&field, px(1), px(1));
            paint_round_rect(dc, &field, UI_SURFACE,
                             GetFocus() == input ? RGB(86,160,255) : UI_PANEL_LINE, 10);
        }
        dot = CreateSolidBrush(g_busy ? UI_COMPRESSION : !g_input_valid || g_calculation_failed ?
                              UI_COMPRESSION_INK : g_model && g_model->solved ? UI_TENSION_INK : UI_MUTED);
        oldbrush = SelectObject(dc, dot); oldpen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, px(21), rect.bottom - px(20), px(27), rect.bottom - px(14));
        SelectObject(dc, oldbrush); SelectObject(dc, oldpen); DeleteObject(dot);
        EndPaint(window, &paint); return 0;
    }
    case WM_CLOSE:
        if (g_busy) {
            g_close_pending = TRUE;
            set_status(L"正在完成当前计算，完成后自动关闭。请稍候…");
        } else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        /* 常规路径线程已在 WM_SOLVE_DONE 收束，这里同时保护外部销毁情况。 */
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        if (g_task) { ui_free_model(g_task->model); free(g_task); g_task = NULL; }
        ui_free_model(g_model); g_model = NULL;
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command, int show)
{
    WNDCLASSEXW cls; INITCOMMONCONTROLSEX common; MSG message = {0};
    RECT area; int width, height, exit_code = 1; HDC screen;
    HMODULE user = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *SetDpiContext)(HANDLE);
    SetDpiContext set_dpi = (SetDpiContext)(void *)GetProcAddress(user, "SetProcessDpiAwarenessContext");
    (void)previous; (void)command;
    if (set_dpi) set_dpi((HANDLE)(INT_PTR)-4); else SetProcessDPIAware();
    screen = GetDC(NULL); g_dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(NULL, screen);
    g_instance = instance;
    GetModuleFileNameW(NULL, g_program_dir, MAX_PATH);
    { wchar_t *slash = wcsrchr(g_program_dir, L'\\'); if (slash) *slash = 0; }
#ifdef TRUSS_STANDALONE
    {
        wchar_t error[1024] = {0};
        wcsncpy_s(g_export_dir, MAX_PATH, g_program_dir, _TRUNCATE);
        if (!ui_prepare_standalone(instance, g_program_dir, MAX_PATH, error, 1024)) {
            MessageBoxW(NULL, error[0] ? error : L"无法准备单文件运行资源，请重新打开程序。",
                        L"平面桁架 · 启动未完成", MB_OK | MB_ICONERROR);
            ui_release_standalone();
            return 1;
        }
    }
#endif
    common.dwSize = sizeof(common); common.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&common); create_fonts();
    g_bg_brush = CreateSolidBrush(UI_BG); g_surface_brush = CreateSolidBrush(UI_SURFACE);
    g_panel_brush = CreateSolidBrush(UI_PANEL); g_app_icon = create_app_icon();
    ZeroMemory(&cls, sizeof(cls)); cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = canvas_proc; cls.lpszClassName = L"TrussCanvas";
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW); cls.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&cls);
    cls.lpfnWndProc = window_proc; cls.lpszClassName = L"TrussWorkbench";
    cls.hIcon = g_app_icon ? g_app_icon : LoadIconW(NULL, IDI_APPLICATION); cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    width = min(px(1200), area.right - area.left - px(20));
    height = min(px(840), area.bottom - area.top - px(20));
    g_window = CreateWindowExW(0, cls.lpszClassName, L"平面桁架", WS_OVERLAPPEDWINDOW |
          WS_CLIPCHILDREN, area.left + (area.right - area.left - width) / 2,
          area.top + (area.bottom - area.top - height) / 2, width, height,
          NULL, NULL, instance, NULL);
    if (!g_window) goto cleanup;
    ShowWindow(g_window, show); UpdateWindow(g_window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        /* 全局快捷键在输入框中也有效；输入期间不会被 Enter 意外计算。 */
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == VK_F5) { calculate(); continue; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && message.wParam == 'O') { import_model(); continue; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && message.wParam == 'S') {
                export_file((GetKeyState(VK_SHIFT) & 0x8000) != 0); continue;
            }
        }
        if (!IsDialogMessageW(g_window, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    exit_code = (int)message.wParam;
    /* 包括异常退出消息循环的情形，先收束计算线程再清理单文件资源。 */
    if (IsWindow(g_window)) DestroyWindow(g_window);
cleanup:
    DeleteObject(g_font); DeleteObject(g_title_font); DeleteObject(g_section_font);
    DeleteObject(g_small_font); DeleteObject(g_bg_brush); DeleteObject(g_surface_brush);
    DeleteObject(g_number_font); DeleteObject(g_raw_font); DeleteObject(g_panel_brush);
    if (g_row_images) ImageList_Destroy(g_row_images);
    if (g_app_icon) DestroyIcon(g_app_icon);
#ifdef TRUSS_STANDALONE
    ui_release_standalone();
#endif
    return exit_code;
}
