/* 原生图形界面的共享数据和模块接口。
 * 界面只组织输入和显示结果，数值求解由同目录 truss.exe 完成。
 * 所有坐标/位移用 m，力用 kN，E 用 kN/m^2，A 用 m^2。
 */
#ifndef TRUSS_UI_H
#define TRUSS_UI_H
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

typedef struct {
    int id, fix_x, fix_y;
    double x, y, prescribed_ux, prescribed_uy, fx, fy;
    double ux, uy, rx, ry;
} UINode;

typedef struct {
    int id, ni, nj;              /* ni/nj 是 nodes 数组下标，输出仍用节点 id。 */
    double E, A, length, c, s;
    double axial, extension, stress;
} UIElement;

typedef struct {
    int n, m, free_dof, solved, required;
    UINode *nodes;
    UIElement *elements;
    double l, h;
    double max_displacement, max_axial, residual, relative_residual;
    double energy, work, energy_error, balance[3];
    wchar_t name[160];
    char *raw_output;           /* 求解器 ASCII 输出原文，导出时保留原始精度。 */
} UIModel;

/* 输入/求解模块；返回 FALSE 时 error 保存中文错误说明。
 * 函数会先完成检查，再替换成功的数据；UIModel 指针初始化必须为 NULL。
 */
UIModel *ui_required_model(double l, double h, wchar_t *error, size_t cap);
UIModel *ui_load_model(const wchar_t *path, wchar_t *error, size_t cap);
UIModel *ui_clone_model(const UIModel *source);
void ui_free_model(UIModel *model);
BOOL ui_save_model(const UIModel *model, const wchar_t *path, wchar_t *error, size_t cap);
BOOL ui_solve_model(UIModel *model, const wchar_t *solver_path,
                    wchar_t *error, size_t cap);
BOOL ui_save_results(const UIModel *model, const wchar_t *path,
                     wchar_t *error, size_t cap);

#ifdef TRUSS_STANDALONE
/* 单文件版仅在独立临时目录释放内置核心和算例；退出时移除自身资源。 */
BOOL ui_prepare_standalone(HINSTANCE instance, wchar_t *program_dir, size_t cap,
                          wchar_t *error, size_t errorcap);
void ui_release_standalone(void);
#endif

/* 绘图模块：geometry=0，轴力=1，放大变形=2。
 * 图采用实际坐标比例；变形模式才有明确标示的视觉放大。
 */
typedef struct {
    int mode, selected_element, show_numbers;
    double zoom, pan_x, pan_y;
} UIView;
void ui_draw_structure(HDC dc, const RECT *rect, const UIModel *model,
                       const UIView *view, int dpi);
int ui_hit_element(const RECT *rect, const UIModel *model,
                    const UIView *view, int x, int y);

/* 原生控件 ID 保持稳定，便于键盘导航及界面验证。 */
#define IDC_PRESET 101
#define IDC_LENGTH 102
#define IDC_HEIGHT 103
#define IDC_CALCULATE 104
#define IDC_OPEN_MODEL 105
#define IDC_SAVE_MODEL 106
#define IDC_SAVE_RESULTS 107
#define IDC_VIEW_GEOMETRY 201
#define IDC_VIEW_AXIAL 202
#define IDC_VIEW_DEFORMED 203
#define IDC_FIT_VIEW 204
#define IDC_RESULT_TAB 301
#define IDC_RESULT_TABLE 302
#define IDC_RAW_OUTPUT 303
#define IDC_STATUS_TEXT 304

/* 工程工作台色板：深蓝模型栏，浅色画布，蓝色操作强调；拉青压橙。 */
#define UI_BG RGB(237,242,246)
#define UI_SURFACE RGB(255,255,255)
#define UI_INK RGB(24,44,64)
#define UI_MUTED RGB(98,114,130)
#define UI_LINE RGB(223,231,239)
#define UI_ACCENT RGB(22,102,211)
#define UI_TENSION RGB(0,141,139)
#define UI_COMPRESSION RGB(214,117,49)
#define UI_TENSION_INK RGB(0,112,111)
#define UI_COMPRESSION_INK RGB(165,78,31)
#define UI_SOFT RGB(245,248,251)
#define UI_PANEL RGB(22,39,59)
#define UI_PANEL_TEXT RGB(217,228,239)
#define UI_PANEL_MUTED RGB(146,169,194)
#define UI_PANEL_LINE RGB(49,70,93)

#endif
