/* 平面桁架图形显示。这里只把已知模型/结果转换成 GDI 图形，不做求解。
 * 所有视图和鼠标拾取共用一个等比例坐标变换，屏幕 y 轴与结构 y 轴相反。
 * 荷载单位 kN，几何和位移单位 m；轴力正值为拉力，负值为压力。
 */
#include "truss_ui.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <wchar.h>

typedef struct {
    double world_x, world_y;    /* 原结构包围盒的中心。 */
    double screen_x, screen_y;  /* 缩放、平移后中心的屏幕坐标。 */
    double scale, fit_scale;
    double amplification;
} UITransform;

typedef struct {
    HDC dc;
    RECT bounds;
    HFONT normal, small, strong;
    RECT labels[256];          /* 小模型避免编号、力值和荷载文字相互遮挡。 */
    RECT obstacles[256];       /* 为节点、支座和荷载箭杆留出净空。 */
    int label_count, obstacle_count, label_top;
} UIDraw;

static double ui_max(double a, double b) { return a > b ? a : b; }
static double ui_min(double a, double b) { return a < b ? a : b; }
static int ui_round(double a) { return (int)floor(a + 0.5); }
static int ui_int_max(int a, int b) { return a > b ? a : b; }

static int ui_legend_height(const RECT *r, const UIModel *model, const UIView *view)
{
    return model && model->solved && view && view->mode == 2 &&
           r->right-r->left < 470 ? 60 : 36;
}

static int ui_load_length(const RECT *r)
{
    return r->bottom-r->top >= 300 ? 43 : 26;
}

/* 对单点、水平杆和竖直杆也保留合理的可视范围。
 * x/y 使用相同 scale；绝不为填满窗口而改变三角形的长宽比。
 */
static int ui_transform(const RECT *r, const UIModel *model,
                        const UIView *view, UITransform *t)
{
    double xmin, xmax, ymin, ymax, spanx, spany, span, width, height;
    double left, right, top, bottom, zoom, max_u = 0.0;
    int i;
    if (!r || !model || !model->nodes || model->n < 1 || !t) return 0;
    width = r->right - r->left;
    height = r->bottom - r->top;
    if (width < 8.0 || height < 8.0) return 0;
    xmin = xmax = model->nodes[0].x;
    ymin = ymax = model->nodes[0].y;
    if (!isfinite(xmin) || !isfinite(ymin)) return 0;
    for (i = 1; i < model->n; ++i) {
        const UINode *node = &model->nodes[i];
        if (!isfinite(node->x) || !isfinite(node->y)) return 0;
        xmin = ui_min(xmin, node->x); xmax = ui_max(xmax, node->x);
        ymin = ui_min(ymin, node->y); ymax = ui_max(ymax, node->y);
    }
    spanx = xmax - xmin; spany = ymax - ymin;
    span = ui_max(spanx, spany);
    if (span <= DBL_MIN || !isfinite(span)) span = 1.0;
    if (spanx <= span * 1e-12) spanx = span * 0.45;
    if (spany <= span * 1e-12) spany = span * 0.45;
    left = ui_min(72.0, width*0.15);
    right = ui_min(92.0, width*0.20);
    /* 上缘留出图例及荷载箭头，下缘留出支座和尺寸；其余空间交给结构。 */
    top = ui_min(88.0, height*0.29);
    top = ui_max(top, ui_legend_height(r,model,view)+ui_load_length(r)+9.0);
    bottom = ui_min(model->required ? 68.0 : 54.0, height*0.26);
    t->fit_scale = ui_min(ui_max(12.0, width-left-right) / spanx,
                         ui_max(12.0, height-top-bottom) / spany);
    zoom = view && isfinite(view->zoom) && view->zoom > 0 ? view->zoom : 1.0;
    zoom = ui_max(0.1, ui_min(16.0, zoom));
    t->world_x = xmin + (xmax - xmin) * 0.5;
    t->world_y = ymin + (ymax - ymin) * 0.5;
    t->screen_x = r->left + left + (width-left-right) * 0.5;
    t->screen_y = r->top + top + (height-top-bottom) * 0.5;
    t->screen_x += view && isfinite(view->pan_x) ? view->pan_x : 0;
    t->screen_y += view && isfinite(view->pan_y) ? view->pan_y : 0;
    t->scale = t->fit_scale * zoom;
    t->amplification = 0.0;
    if (model->solved && view && view->mode == 2) {
        for (i = 0; i < model->n; ++i) {
            double u = hypot(model->nodes[i].ux, model->nodes[i].uy);
            if (isfinite(u)) max_u = ui_max(max_u, u);
        }
        /* 放大后最大位移约为视图短边的 7%。上限保证不会出现无限倍。
         * 用 fit_scale 决定放大率，随后 zoom 同时缩放结构和变形。
         */
        if (max_u > DBL_MIN) {
            double visible_u = ui_min(width, height) * 0.07;
            t->amplification = ui_max(1.0, ui_min(1e8,
                                          visible_u / max_u / t->fit_scale));
        }
    }
    return isfinite(t->scale) && t->scale > 0;
}

static POINT ui_position(const UITransform *t, const UINode *node, int deformed)
{
    double x = node->x, y = node->y;
    POINT p;
    if (deformed && t->amplification > 0) {
        x += node->ux * t->amplification;
        y += node->uy * t->amplification;
    }
    /* GDI 的坐标受 32 位整数限制；异常大的离屏坐标不应溢出。 */
    x = t->screen_x + (x - t->world_x) * t->scale;
    y = t->screen_y - (y - t->world_y) * t->scale;
    p.x = ui_round(ui_max(-1000000.0, ui_min(1000000.0, x)));
    p.y = ui_round(ui_max(-1000000.0, ui_min(1000000.0, y)));
    return p;
}

static void ui_line(HDC dc, int x1, int y1, int x2, int y2,
                    COLORREF color, int width, int style)
{
    HPEN pen = CreatePen(style, width, color);
    HGDIOBJ previous;
    if (!pen) return;
    previous = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, NULL); LineTo(dc, x2, y2);
    SelectObject(dc, previous); DeleteObject(pen);
}

/* 箭头的终点就是受力方向，不把屏幕向下误认为结构的正 y。 */
static void ui_arrow(HDC dc, int x1, int y1, int x2, int y2,
                     COLORREF color, int width, double head)
{
    double dx = x2-x1, dy = y2-y1, length = hypot(dx, dy), ux, uy;
    if (length < 1.0) return;
    ux = dx/length; uy = dy/length;
    ui_line(dc, x1, y1, x2, y2, color, width, PS_SOLID);
    ui_line(dc, x2, y2, ui_round(x2-head*ux+head*0.48*uy),
            ui_round(y2-head*uy-head*0.48*ux), color, width, PS_SOLID);
    ui_line(dc, x2, y2, ui_round(x2-head*ux-head*0.48*uy),
            ui_round(y2-head*uy+head*0.48*ux), color, width, PS_SOLID);
}

static void ui_circle(HDC dc, int x, int y, int radius,
                      COLORREF border, COLORREF fill, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, border);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ old_pen, old_brush;
    if (!pen || !brush) {
        if (pen) DeleteObject(pen); if (brush) DeleteObject(brush);
        return;
    }
    old_pen = SelectObject(dc, pen); old_brush = SelectObject(dc, brush);
    Ellipse(dc, x-radius, y-radius, x+radius+1, y+radius+1);
    SelectObject(dc, old_pen); SelectObject(dc, old_brush);
    DeleteObject(pen); DeleteObject(brush);
}

static int ui_overlap_area(const RECT *a, const RECT *b)
{
    int width = (int)ui_min(a->right+4,b->right+4) -
                (int)ui_max(a->left,b->left);
    int height = (int)ui_min(a->bottom+3,b->bottom+3) -
                 (int)ui_max(a->top,b->top);
    return width > 0 && height > 0 ? width*height : 0;
}

/* 编号贴近节点时，避开相邻杆件，而不将斜杆的整个包围盒视为障碍。 */
static int ui_segment_hits_rect(const RECT *r, POINT a, POINT b)
{
    double dx=b.x-a.x, dy=b.y-a.y, lo=0.0, hi=1.0;
    double p[4]={-dx,dx,-dy,dy};
    double q[4]={a.x-r->left,r->right-a.x,a.y-r->top,r->bottom-a.y};
    int i;
    for (i=0;i<4;++i) {
        if (fabs(p[i])<1e-12) {
            if (q[i]<0) return 0;
        } else {
            double u=q[i]/p[i];
            if (p[i]<0) lo=ui_max(lo,u); else hi=ui_min(hi,u);
            if (lo>hi) return 0;
        }
    }
    return 1;
}

static void ui_obstacle(UIDraw *d, int x1, int y1, int x2, int y2, int pad)
{
    RECT *r;
    if (d->obstacle_count >= (int)(sizeof(d->obstacles)/sizeof(d->obstacles[0]))) return;
    r = &d->obstacles[d->obstacle_count++];
    r->left = (LONG)ui_min(x1,x2)-pad; r->right = (LONG)ui_max(x1,x2)+pad;
    r->top = (LONG)ui_min(y1,y2)-pad; r->bottom = (LONG)ui_max(y1,y2)+pad;
}

/* 标签以白底隔开网格及杆线；候选位置优先保持靠近该杆/节点。
 * 图元先绘制、标签后绘制，因此小字始终清晰可读。
 */
static void ui_label(UIDraw *d, const wchar_t *text, int center_x, int center_y,
                     HFONT font, COLORREF color, int boxed, int selected)
{
    static const int offsets[][2] = {
        {0,0},{0,-22},{0,22},{-28,0},{28,0},{-24,-22},{24,-22},
        {-24,22},{24,22},{0,-44},{0,44},{-52,0},{52,0},
        {-44,-38},{44,-38},{-44,38},{44,38}
    };
    HGDIOBJ old_font = SelectObject(d->dc, font);
    SIZE size; RECT r = {0,0,0,0}, best = {0,0,0,0};
    int width, height, i, j, score, best_score = 0x7fffffff;
    double spacing;
    GetTextExtentPoint32W(d->dc, text, (int)wcslen(text), &size);
    width = size.cx + (boxed ? 12 : 6);
    height = size.cy + (boxed ? 6 : 2);
    spacing = ui_max(1.0,size.cy/15.0);
    for (i = 0; i < (int)(sizeof(offsets)/sizeof(offsets[0])); ++i) {
        int x = center_x + ui_round(offsets[i][0]*spacing);
        int y = center_y + ui_round(offsets[i][1]*spacing);
        r.left = x-width/2; r.right = r.left+width;
        r.top = y-height/2; r.bottom = r.top+height;
        if (r.left < d->bounds.left+7) OffsetRect(&r, d->bounds.left+7-r.left, 0);
        if (r.right > d->bounds.right-7) OffsetRect(&r, d->bounds.right-7-r.right, 0);
        if (r.top < d->label_top+4) OffsetRect(&r, 0, d->label_top+4-r.top);
        if (r.bottom > d->bounds.bottom-12) OffsetRect(&r, 0, d->bounds.bottom-12-r.bottom);
        score = (offsets[i][0]*offsets[i][0]+offsets[i][1]*offsets[i][1])/36;
        for (j = 0; j < d->label_count; ++j) {
            int area = ui_overlap_area(&r,&d->labels[j]);
            if (area) score += 1200+area*8;
        }
        for (j = 0; j < d->obstacle_count; ++j) {
            int area = ui_overlap_area(&r,&d->obstacles[j]);
            if (area) score += 600+area*5;
        }
        if (score < best_score) { best_score = score; best = r; }
        if (!score) break;
    }
    r = best;
    if (d->label_count < (int)(sizeof(d->labels)/sizeof(d->labels[0])))
        d->labels[d->label_count++] = r;
    if (boxed) {
        HBRUSH brush = CreateSolidBrush(selected ? RGB(235,243,255) : UI_SURFACE);
        HPEN pen = CreatePen(PS_SOLID, 1, selected ? RGB(116,168,238) : UI_LINE);
        HGDIOBJ old_brush = SelectObject(d->dc, brush);
        HGDIOBJ old_pen = SelectObject(d->dc, pen);
        RoundRect(d->dc, r.left, r.top, r.right, r.bottom, 8, 8);
        SelectObject(d->dc, old_brush); SelectObject(d->dc, old_pen);
        DeleteObject(brush); DeleteObject(pen);
    } else {
        HBRUSH brush = CreateSolidBrush(UI_SURFACE);
        FillRect(d->dc, &r, brush); DeleteObject(brush);
    }
    SetTextColor(d->dc, color); SetBkMode(d->dc, TRANSPARENT);
    DrawTextW(d->dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d->dc, old_font);
}

/* 节点编号始终锚定在节点周围。它不避让自身支座的大包围盒，
 * 也不复用杆力标签的远距离候选位；后画的杆力标签会避让这些编号。
 */
static void ui_node_label(UIDraw *d, const UIModel *model, const UITransform *t,
                          int index, int deformed)
{
    const UINode *node=&model->nodes[index];
    POINT p=ui_position(t,node,deformed);
    wchar_t number[32]; SIZE size;
    RECT r={0,0,0,0}, best={0,0,0,0}, load;
    HGDIOBJ old_font=SelectObject(d->dc,d->strong);
    HBRUSH background;
    int candidates[8][2];
    int sx=node->x<=t->world_x ? -1 : 1;
    int sy=(fabs(node->fx)+fabs(node->fy)>1e-12) ? 1 : -1;
    int dx,dy,width,height,i,j,score,best_score=0x7fffffff;
    int load_length=ui_load_length(&d->bounds);
    swprintf(number,sizeof(number)/sizeof(number[0]),L"%d",node->id);
    GetTextExtentPoint32W(d->dc,number,(int)wcslen(number),&size);
    width=size.cx+6; height=size.cy+2;
    dx=width/2+9; dy=height/2+9;
    candidates[0][0]=sx*dx; candidates[0][1]=sy*dy;
    candidates[1][0]=-sx*dx; candidates[1][1]=sy*dy;
    candidates[2][0]=sx*dx; candidates[2][1]=-sy*dy;
    candidates[3][0]=-sx*dx; candidates[3][1]=-sy*dy;
    candidates[4][0]=0; candidates[4][1]=sy*dy;
    candidates[5][0]=0; candidates[5][1]=-sy*dy;
    candidates[6][0]=sx*dx; candidates[6][1]=0;
    candidates[7][0]=-sx*dx; candidates[7][1]=0;
    for (i=0;i<8;++i) {
        r.left=p.x+candidates[i][0]-width/2; r.right=r.left+width;
        r.top=p.y+candidates[i][1]-height/2; r.bottom=r.top+height;
        if (r.left<d->bounds.left+7) OffsetRect(&r,d->bounds.left+7-r.left,0);
        if (r.right>d->bounds.right-7) OffsetRect(&r,d->bounds.right-7-r.right,0);
        if (r.top<d->label_top+4) OffsetRect(&r,0,d->label_top+4-r.top);
        if (r.bottom>d->bounds.bottom-12) OffsetRect(&r,0,d->bounds.bottom-12-r.bottom);
        score=i*6;
        for (j=0;j<d->label_count;++j) {
            int area=ui_overlap_area(&r,&d->labels[j]);
            if (area) score+=1200+area*8;
        }
        for (j=0;j<model->n;++j) {
            POINT other; RECT node_bounds; int area;
            if (j==index) continue;
            other=ui_position(t,&model->nodes[j],deformed);
            node_bounds=(RECT){other.x-9,other.y-9,other.x+9,other.y+9};
            area=ui_overlap_area(&r,&node_bounds);
            if (area) score+=600+area*5;
        }
        for (j=0;j<model->m;++j) {
            const UIElement *e=&model->elements[j];
            POINT other, start, end; double vx,vy,length;
            int neighbor=e->ni==index ? e->nj : e->nj==index ? e->ni : -1;
            if (neighbor<0 || neighbor>=model->n) continue;
            other=ui_position(t,&model->nodes[neighbor],deformed);
            vx=other.x-p.x; vy=other.y-p.y; length=hypot(vx,vy);
            if (length<1.0) continue;
            start.x=p.x+ui_round(vx/length*7); start.y=p.y+ui_round(vy/length*7);
            end.x=p.x+ui_round(vx/length*ui_min(38.0,length));
            end.y=p.y+ui_round(vy/length*ui_min(38.0,length));
            if (ui_segment_hits_rect(&r,start,end)) score+=700;
        }
        if (isfinite(node->fx) && fabs(node->fx)>1e-12) {
            int direction=node->fx>0 ? 1 : -1;
            int start=p.x+direction*9, end=p.x+direction*(load_length+9);
            int area;
            load=(RECT){(LONG)ui_min(start,end)-4,p.y-4,
                        (LONG)ui_max(start,end)+4,p.y+4};
            area=ui_overlap_area(&r,&load);
            if (area) score+=600+area*5;
        }
        if (isfinite(node->fy) && fabs(node->fy)>1e-12) {
            int direction=node->fy>0 ? -1 : 1;
            int start=p.y-direction*9, end=p.y-direction*(load_length+9);
            int area;
            load=(RECT){p.x-4,(LONG)ui_min(start,end)-4,
                        p.x+4,(LONG)ui_max(start,end)+4};
            area=ui_overlap_area(&r,&load);
            if (area) score+=600+area*5;
        }
        if (score<best_score) { best_score=score; best=r; }
        if (!score) break;
    }
    if (d->label_count<(int)(sizeof(d->labels)/sizeof(d->labels[0])))
        d->labels[d->label_count++]=best;
    background=CreateSolidBrush(UI_SURFACE);
    FillRect(d->dc,&best,background); DeleteObject(background);
    SetTextColor(d->dc,UI_ACCENT); SetBkMode(d->dc,TRANSPARENT);
    DrawTextW(d->dc,number,-1,&best,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(d->dc,old_font);
}

static void ui_plain_text(HDC dc, HFONT font, COLORREF color,
                          const wchar_t *text, RECT *r, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, r, flags);
    SelectObject(dc, old);
}

/* 与原题一致，一个约束方向对应一个滚动支座。
 * fix_x 的墙面竖直，fix_y 的支承面水平；双固定节点显示两者。
 */
static void ui_support(UIDraw *d, const UITransform *t,
                       const UINode *node, POINT p)
{
    int side_x = node->x <= t->world_x ? -1 : 1;
    int side_y = node->y <= t->world_y ? 1 : -1;
    int i;
    if (node->fix_x) {
        int wheel_x = p.x+side_x*22, wall_x = p.x+side_x*28;
        ui_line(d->dc, p.x, p.y, wheel_x-side_x*3, p.y, UI_MUTED, 1, PS_SOLID);
        ui_circle(d->dc, wheel_x, p.y, 3, UI_MUTED, UI_SURFACE, 1);
        ui_line(d->dc, wall_x, p.y-13, wall_x, p.y+13, UI_MUTED, 1, PS_SOLID);
        for (i = -12; i <= 12; i += 6)
            ui_line(d->dc, wall_x, p.y+i, wall_x+side_x*6,
                    p.y+i-4, UI_LINE, 1, PS_SOLID);
    }
    if (node->fix_y) {
        int wheel_y = p.y+side_y*22, ground_y = p.y+side_y*28;
        ui_line(d->dc, p.x, p.y, p.x, wheel_y-side_y*3, UI_MUTED, 1, PS_SOLID);
        ui_circle(d->dc, p.x, wheel_y, 3, UI_MUTED, UI_SURFACE, 1);
        ui_line(d->dc, p.x-13, ground_y, p.x+13, ground_y, UI_MUTED, 1, PS_SOLID);
        for (i = -12; i <= 12; i += 6)
            ui_line(d->dc, p.x+i, ground_y, p.x+i-4,
                    ground_y+side_y*6, UI_LINE, 1, PS_SOLID);
    }
}

static const UINode *ui_node_id(const UIModel *model, int id)
{
    int i;
    for (i = 0; i < model->n; ++i)
        if (model->nodes[i].id == id) return &model->nodes[i];
    return NULL;
}

static void ui_dimensions(UIDraw *d, const UIModel *model, const UITransform *t,
                          int labels)
{
    const UINode *n1, *n2, *n4;
    POINT a, b, top; int line_y, line_x;
    wchar_t text[96];
    if (!model->required) return;
    n1 = ui_node_id(model, 1); n2 = ui_node_id(model, 2); n4 = ui_node_id(model, 4);
    if (!n1 || !n2 || !n4) return;
    a = ui_position(t, n1, 0); b = ui_position(t, n2, 0);
    top = ui_position(t, n4, 0);
    line_y = (int)ui_min(ui_int_max(a.y,b.y)+51,d->bounds.bottom-32);
    line_x = (int)ui_min(ui_int_max(a.x,b.x)+44,d->bounds.right-53);
    if (!labels) {
        ui_line(d->dc, a.x, a.y+35, a.x, line_y+5, UI_LINE, 1, PS_SOLID);
        ui_line(d->dc, b.x, b.y+35, b.x, line_y+5, UI_LINE, 1, PS_SOLID);
        ui_arrow(d->dc, b.x, line_y, a.x, line_y, UI_MUTED, 1, 4);
        ui_arrow(d->dc, a.x, line_y, b.x, line_y, UI_MUTED, 1, 4);
        ui_line(d->dc, top.x+10, top.y, line_x+5, top.y, UI_LINE, 1, PS_SOLID);
        ui_line(d->dc, b.x+35, b.y, line_x+5, b.y, UI_LINE, 1, PS_SOLID);
        ui_arrow(d->dc, line_x, b.y, line_x, top.y, UI_MUTED, 1, 4);
        ui_arrow(d->dc, line_x, top.y, line_x, b.y, UI_MUTED, 1, 4);
    } else {
        swprintf(text, sizeof(text)/sizeof(text[0]), L"l %.4g m", model->l);
        ui_label(d, text, (a.x+b.x)/2, line_y, d->small, UI_MUTED, 0, 0);
        swprintf(text, sizeof(text)/sizeof(text[0]), L"h %.4g m", model->h);
        ui_label(d, text, line_x+28, (top.y+b.y)/2, d->small, UI_MUTED, 0, 0);
    }
}

/* 用分量画荷载，正 Fx 向右、负 Fy 向下。
 * 在顶节点负 Fy 的箭杆放在节点上方，避免压住右斜杆；箭头仍向下。
 */
static void ui_load(UIDraw *d, const UINode *node, POINT p, int labels)
{
    wchar_t text[96];
    int direction, near_offset = 9;
    int length = ui_load_length(&d->bounds);
    COLORREF load_color = UI_ACCENT;
    if (isfinite(node->fx) && fabs(node->fx) > 1e-12) {
        direction = node->fx > 0 ? 1 : -1;
        if (!labels)
            ui_arrow(d->dc, p.x+direction*near_offset, p.y,
                     p.x+direction*(near_offset+length), p.y, load_color, 2, 7);
        else {
            swprintf(text, sizeof(text)/sizeof(text[0]), L"Fx %+.4g kN", node->fx);
            ui_label(d, text, p.x+direction*(near_offset+length/2), p.y-19,
                     d->small, load_color, 0, 0);
        }
    }
    if (isfinite(node->fy) && fabs(node->fy) > 1e-12) {
        direction = node->fy > 0 ? -1 : 1; /* 世界正 y 是屏幕向上。 */
        /* 箭头由离节点较远处指向节点，避免箭杆跨入结构内部。 */
        if (!labels)
            ui_arrow(d->dc, p.x, p.y-direction*(near_offset+length),
                     p.x, p.y-direction*near_offset, load_color, 2, 7);
        else {
            swprintf(text, sizeof(text)/sizeof(text[0]), L"Fy %+.4g kN", node->fy);
            ui_label(d, text, p.x-57, p.y-direction*(length-6),
                     d->small, load_color, 0, 0);
        }
    }
}

static COLORREF ui_axial_color(double axial, double max_axial)
{
    double zero = ui_max(1e-9, max_axial*1e-10);
    if (fabs(axial) <= zero) return UI_MUTED;
    return axial > 0 ? UI_TENSION : UI_COMPRESSION;
}

static double ui_model_max_axial(const UIModel *model)
{
    double max_axial = 0;
    int i;
    for (i = 0; i < model->m; ++i)
        if (isfinite(model->elements[i].axial))
            max_axial = ui_max(max_axial, fabs(model->elements[i].axial));
    return max_axial;
}

static int ui_valid_element(const UIModel *model, const UIElement *e)
{
    return e->ni >= 0 && e->nj >= 0 && e->ni < model->n && e->nj < model->n;
}

static void ui_heading(UIDraw *d, const UIModel *model, const UIView *view,
                       const UITransform *t)
{
    wchar_t text[128]; RECT area = d->bounds;
    HBRUSH white = CreateSolidBrush(UI_SURFACE);
    int width = d->bounds.right-d->bounds.left;
    int x = d->bounds.left+18, y = d->bounds.top+19;
    area.bottom = d->label_top;
    FillRect(d->dc, &area, white); DeleteObject(white);
    area.top = y-13; area.bottom = y+13;
    if (view->mode == 1 && model->solved) {
        ui_line(d->dc,x,y,x+17,y,UI_TENSION,3,PS_SOLID);
        area.left=x+25; area.right=x+84;
        ui_plain_text(d->dc,d->small,UI_MUTED,L"拉力 +",&area,DT_SINGLELINE|DT_VCENTER);
        x += 97;
        ui_line(d->dc,x,y,x+17,y,UI_COMPRESSION,3,PS_SOLID);
        area.left=x+25; area.right=x+87;
        ui_plain_text(d->dc,d->small,UI_MUTED,L"压力 −",&area,DT_SINGLELINE|DT_VCENTER);
        area.left=d->bounds.right-50; area.right=d->bounds.right-18;
        ui_plain_text(d->dc,d->small,UI_MUTED,L"kN",&area,
                      DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    } else if (view->mode == 2 && model->solved) {
        swprintf(text,sizeof(text)/sizeof(text[0]),
                 t->amplification > 0 ? L"变形放大 %.4g 倍" : L"变形放大 %.4g 倍 · 位移为零",
                 t->amplification > 0 ? t->amplification : 1.0);
        area.left=x;
        area.right=width >= 470 ? d->bounds.right-220 : d->bounds.right-18;
        ui_plain_text(d->dc,d->small,UI_INK,text,&area,
                      DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        if (width >= 470) x=d->bounds.right-207;
        else { x=d->bounds.left+18; y=d->bounds.top+44; area.top=y-12; area.bottom=y+12; }
        ui_line(d->dc,x,y,x+19,y,RGB(166,183,199),1,PS_DASH);
        area.left=x+27; area.right=x+91;
        ui_plain_text(d->dc,d->small,UI_MUTED,L"原结构",&area,DT_SINGLELINE|DT_VCENTER);
        x += 105;
        ui_line(d->dc,x,y,x+19,y,UI_ACCENT,2,PS_SOLID);
        area.left=x+27; area.right=x+85;
        ui_plain_text(d->dc,d->small,UI_MUTED,L"变形",&area,DT_SINGLELINE|DT_VCENTER);
    } else {
        area.left=d->bounds.right-110; area.right=d->bounds.right-18;
        ui_plain_text(d->dc,d->small,UI_MUTED,L"m / kN",&area,
                      DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    }
}

void ui_draw_structure(HDC dc, const RECT *rect, const UIModel *model,
                       const UIView *view, int dpi)
{
    UIView fallback = {0,-1,1,1.0,0.0,0.0};
    UIDraw d; UITransform t; RECT text_rect;
    HBRUSH background;
    double max_axial; int saved, i, x, y, size, deformed, show_axial;
    if (!dc || !rect) return;
    saved = SaveDC(dc);
    IntersectClipRect(dc,rect->left,rect->top,rect->right,rect->bottom);
    background = CreateSolidBrush(UI_SURFACE);
    FillRect(dc,rect,background); DeleteObject(background);
    if (!view) view = &fallback;
    ZeroMemory(&d,sizeof(d)); d.dc=dc; d.bounds=*rect;
    d.label_top = rect->top+ui_legend_height(rect,model,view);
    /* 图注与主界面遵循同一 DPI，窗口变小时仍保持可读字号。 */
    size = MulDiv(12, dpi > 0 ? dpi : 96, 96);
    if (size < 12) size = 12;
    d.normal=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    d.small=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    d.strong=CreateFontW(-size,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if (!ui_transform(rect,model,view,&t)) {
        text_rect=*rect;
        ui_plain_text(dc,d.normal,UI_MUTED,L"导入模型或载入示例",
            &text_rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        goto cleanup;
    }
    /* 点阵只提供绘图背景，不表示长度刻度，也不与杆线争抢视觉层次。 */
    for (x=rect->left+24;x<rect->right-12;x+=28)
        for (y=d.label_top+16;y<rect->bottom-12;y+=28)
            SetPixelV(dc,x,y,RGB(226,233,240));
    deformed=view->mode==2 && model->solved;
    show_axial=view->mode==1 && model->solved;
    max_axial=show_axial ? ui_model_max_axial(model) : 0.0;
    ui_obstacle(&d,rect->left+13,rect->bottom-68,rect->left+66,rect->bottom-12,0);
    for (i=0;i<model->n;++i) {
        const UINode *node=&model->nodes[i];
        POINT p=ui_position(&t,node,deformed), original=ui_position(&t,node,0);
        int direction, load_length=ui_load_length(rect);
        ui_obstacle(&d,p.x,p.y,p.x,p.y,9);
        if (node->fix_x) {
            direction=node->x<=t.world_x ? -1 : 1;
            ui_obstacle(&d,original.x+direction*10,original.y-16,
                        original.x+direction*35,original.y+16,1);
        }
        if (node->fix_y) {
            direction=node->y<=t.world_y ? 1 : -1;
            ui_obstacle(&d,original.x-16,original.y+direction*10,
                        original.x+16,original.y+direction*35,1);
        }
        if (isfinite(node->fx) && fabs(node->fx)>1e-12) {
            direction=node->fx>0 ? 1 : -1;
            ui_obstacle(&d,p.x+direction*9,p.y,p.x+direction*(load_length+9),p.y,4);
        }
        if (isfinite(node->fy) && fabs(node->fy)>1e-12) {
            direction=node->fy>0 ? -1 : 1;
            ui_obstacle(&d,p.x,p.y-direction*9,p.x,p.y-direction*(load_length+9),4);
        }
    }
    if (deformed) {
        for (i=0;i<model->m;++i) {
            const UIElement *e=&model->elements[i]; POINT a,b;
            if (!ui_valid_element(model,e)) continue;
            a=ui_position(&t,&model->nodes[e->ni],0);
            b=ui_position(&t,&model->nodes[e->nj],0);
            ui_line(dc,a.x,a.y,b.x,b.y,RGB(166,183,199),1,PS_DASH);
        }
        for (i=0;i<model->n;++i) {
            POINT p=ui_position(&t,&model->nodes[i],0);
            ui_circle(dc,p.x,p.y,4,RGB(166,183,199),UI_SURFACE,1);
        }
    }
    for (i=0;i<model->n;++i) {
        POINT p=ui_position(&t,&model->nodes[i],0);
        ui_support(&d,&t,&model->nodes[i],p);
    }
    for (i=0;i<model->m;++i) {
        const UIElement *e=&model->elements[i]; POINT a,b;
        COLORREF color=deformed ? UI_ACCENT : UI_INK;
        int width=2;
        if (!ui_valid_element(model,e)) continue;
        a=ui_position(&t,&model->nodes[e->ni],deformed);
        b=ui_position(&t,&model->nodes[e->nj],deformed);
        if (show_axial) {
            color=ui_axial_color(e->axial,max_axial);
            width=max_axial>0 ? 2+(int)floor(2*fabs(e->axial)/max_axial+0.5) : 1;
            if (width>4) width=4;
        }
        if (i==view->selected_element)
            ui_line(dc,a.x,a.y,b.x,b.y,RGB(211,229,252),width+6,PS_SOLID);
        ui_line(dc,a.x,a.y,b.x,b.y,color,width,PS_SOLID);
    }
    /* 尺寸始终对应原结构，不能把变形放大后的距离作为尺寸。 */
    if (!deformed) ui_dimensions(&d,model,&t,0);
    for (i=0;i<model->n;++i) {
        POINT p=ui_position(&t,&model->nodes[i],deformed);
        ui_load(&d,&model->nodes[i],p,0);
    }
    for (i=0;i<model->n;++i) {
        POINT p=ui_position(&t,&model->nodes[i],deformed);
        ui_circle(dc,p.x,p.y,5,UI_ACCENT,UI_SURFACE,2);
    }
    /* 所有文字置于图元上层，后画的箭杆和节点不会再盖住先画的标签。 */
    if (!deformed) ui_dimensions(&d,model,&t,1);
    for (i=0;i<model->n;++i) {
        POINT p=ui_position(&t,&model->nodes[i],deformed);
        ui_load(&d,&model->nodes[i],p,1);
    }
    if (view->show_numbers)
        for (i=0;i<model->n;++i) ui_node_label(&d,model,&t,i,deformed);
    /* 选中杆的标签先占据最佳位置，其余杆号在附近避让。 */
    for (i=-1;i<model->m;++i) {
        int index=i<0 ? view->selected_element : i;
        const UIElement *e; POINT a,b;
        double dx,dy,length; int offset_x,offset_y;
        wchar_t text[96]; COLORREF color=UI_MUTED;
        if (index<0 || index>=model->m || (i>=0 && i==view->selected_element)) continue;
        e=&model->elements[index];
        if (!ui_valid_element(model,e) || (!view->show_numbers && !show_axial)) continue;
        a=ui_position(&t,&model->nodes[e->ni],deformed);
        b=ui_position(&t,&model->nodes[e->nj],deformed);
        dx=b.x-a.x; dy=b.y-a.y; length=hypot(dx,dy);
        offset_x=length>1 ? ui_round(-dy/length*17) : 0;
        offset_y=length>1 ? ui_round(dx/length*17) : -17;
        if (((a.x+b.x)*0.5-t.screen_x)*offset_x+
            ((a.y+b.y)*0.5-t.screen_y)*offset_y < 0) {
            offset_x=-offset_x; offset_y=-offset_y;
        }
        if (show_axial) {
            double axial=e->axial;
            if (fabs(axial)<=ui_max(1e-9,max_axial*1e-10)) axial=0;
            color=axial>0 ? UI_TENSION_INK : axial<0 ? UI_COMPRESSION_INK : UI_MUTED;
            if (view->show_numbers)
                swprintf(text,sizeof(text)/sizeof(text[0]),L"%d · %+.4g",e->id,axial);
            else swprintf(text,sizeof(text)/sizeof(text[0]),L"%+.4g",axial);
        } else swprintf(text,sizeof(text)/sizeof(text[0]),L"%d",e->id);
        ui_label(&d,text,(a.x+b.x)/2+offset_x,(a.y+b.y)/2+offset_y,
                 d.small,color,show_axial || index==view->selected_element,
                 index==view->selected_element);
    }
    /* 左下角坐标只表示方向；与边长和鼠标缩放无关。 */
    x=rect->left+23; y=rect->bottom-23;
    ui_arrow(dc,x,y,x+31,y,UI_MUTED,1,5);
    ui_arrow(dc,x,y,x,y-27,UI_MUTED,1,5);
    text_rect.left=x+33;text_rect.right=x+47;text_rect.top=y-9;text_rect.bottom=y+10;
    ui_plain_text(dc,d.small,UI_MUTED,L"x",&text_rect,DT_SINGLELINE|DT_CENTER);
    text_rect.left=x-6;text_rect.right=x+9;text_rect.top=y-45;text_rect.bottom=y-27;
    ui_plain_text(dc,d.small,UI_MUTED,L"y",&text_rect,DT_SINGLELINE|DT_CENTER);
    if (!model->solved && view->mode!=0) {
        text_rect=*rect; text_rect.top=rect->bottom-27;
        text_rect.left=rect->left+80; text_rect.right=rect->right-15;
        ui_plain_text(dc,d.small,UI_MUTED,
            view->mode==1 ? L"计算后显示杆件轴力" : L"计算后显示放大变形",
            &text_rect,DT_SINGLELINE|DT_RIGHT|DT_VCENTER);
    }
    ui_heading(&d,model,view,&t);
cleanup:
    if (d.normal) DeleteObject(d.normal);
    if (d.small) DeleteObject(d.small);
    if (d.strong) DeleteObject(d.strong);
    if (saved) RestoreDC(dc,saved);
}

/* 返回最近杆件的数组下标。投影系数限制在 [0,1]，端点也可点选。
 * 变形图使用放大后的节点，确保画在哪里就能在哪里选中。
 */
int ui_hit_element(const RECT *rect, const UIModel *model,
                    const UIView *view, int x, int y)
{
    UIView fallback={0,-1,1,1.0,0.0,0.0};
    UITransform t; double best=100.0; int index=-1, i, deformed;
    if (!rect || x<rect->left || x>=rect->right || y<rect->top || y>=rect->bottom)
        return -1;
    if (!view) view=&fallback;
    if (!ui_transform(rect,model,view,&t) || !model->elements) return -1;
    deformed=view->mode==2 && model->solved;
    for (i=0;i<model->m;++i) {
        const UIElement *e=&model->elements[i];
        POINT a,b; double dx,dy,denom,u,px,py,dist;
        if (!ui_valid_element(model,e)) continue;
        a=ui_position(&t,&model->nodes[e->ni],deformed);
        b=ui_position(&t,&model->nodes[e->nj],deformed);
        dx=b.x-a.x;dy=b.y-a.y;denom=dx*dx+dy*dy;
        u=denom>0 ? ((x-a.x)*dx+(y-a.y)*dy)/denom : 0;
        u=ui_max(0.0,ui_min(1.0,u));
        px=a.x+u*dx;py=a.y+u*dy;
        dist=(x-px)*(x-px)+(y-py)*(y-py);
        if (dist<=best) { best=dist;index=i; }
    }
    return index;
}
