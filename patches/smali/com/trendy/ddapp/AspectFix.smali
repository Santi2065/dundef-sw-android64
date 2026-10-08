# Tablet HUD: on screens of 7" or more (the game's own IsTablet test) the HUD only fits at ~3:2;
# at 7:5 BUILD/HERO fall off the right edge, at 16:10 the bottom row is cut too. Phones get another
# layout that fits any width. So on tablets the whole window is sized to 1.48:1 (verified to fit),
# centered with black around it. Resizing the window, not just the surface, keeps touch, keyboard
# and video coordinates consistent with no translation. Called from ddapp.onCreate.
.class public final Lcom/trendy/ddapp/AspectFix;
.super Ljava/lang/Object;

.method public static apply(Landroid/app/Activity;)V
    .locals 9
    new-instance v0, Landroid/util/DisplayMetrics;
    invoke-direct {v0}, Landroid/util/DisplayMetrics;-><init>()V
    invoke-virtual {p0}, Landroid/app/Activity;->getWindowManager()Landroid/view/WindowManager;
    move-result-object v1
    invoke-interface {v1}, Landroid/view/WindowManager;->getDefaultDisplay()Landroid/view/Display;
    move-result-object v1
    invoke-virtual {v1, v0}, Landroid/view/Display;->getRealMetrics(Landroid/util/DisplayMetrics;)V
    iget v1, v0, Landroid/util/DisplayMetrics;->widthPixels:I
    iget v2, v0, Landroid/util/DisplayMetrics;->heightPixels:I
    # diagonal^2 in inches, computed like the game does (pixels / dpi per axis)
    int-to-float v5, v1
    iget v6, v0, Landroid/util/DisplayMetrics;->xdpi:F
    div-float/2addr v5, v6
    mul-float/2addr v5, v5
    int-to-float v6, v2
    iget v7, v0, Landroid/util/DisplayMetrics;->ydpi:F
    div-float/2addr v6, v7
    mul-float/2addr v6, v6
    add-float/2addr v5, v6
    const/high16 v6, 0x42440000    # 49.0f = 7in squared
    cmpg-float v6, v5, v6
    if-ltz v6, :done
    invoke-static {v1, v2}, Ljava/lang/Math;->max(II)I
    move-result v3                 # landscape width
    invoke-static {v1, v2}, Ljava/lang/Math;->min(II)I
    move-result v4                 # landscape height
    const v7, 0x3fbd70a4           # 1.48f
    int-to-float v5, v4
    mul-float/2addr v5, v7
    float-to-int v5, v5            # width = height * 1.48 (pillarbox)
    move v6, v4
    if-le v5, v3, :sized
    int-to-float v6, v3
    div-float/2addr v6, v7
    float-to-int v6, v6            # too wide: height = width / 1.48 (letterbox)
    move v5, v3
    :sized
    invoke-virtual {p0}, Landroid/app/Activity;->getWindow()Landroid/view/Window;
    move-result-object v7
    invoke-virtual {v7, v5, v6}, Landroid/view/Window;->setLayout(II)V
    const/16 v8, 0x11              # Gravity.CENTER
    invoke-virtual {v7, v8}, Landroid/view/Window;->setGravity(I)V
    const/4 v8, 0x2                # FLAG_DIM_BEHIND, fully dimmed = black bars
    invoke-virtual {v7, v8}, Landroid/view/Window;->addFlags(I)V
    const/high16 v8, 0x3f800000
    invoke-virtual {v7, v8}, Landroid/view/Window;->setDimAmount(F)V
    :done
    return-void
.end method
