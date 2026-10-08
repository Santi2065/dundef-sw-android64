# Replaces the game's EditTextBackEvent (hero / game name input).
# The field was multi-line, so a touch keyboard's Enter typed a newline, and the only accept path was
# tapping the game's button, which the keyboard covers on tablets; Back (closing the keyboard)
# discarded the text. Single-line + "Done" accepts, and so does closing the keyboard.
.class final Lcom/trendy/ddapp/EditTextBackEvent;
.super Landroid/widget/EditText;
.implements Landroid/widget/TextView$OnEditorActionListener;

.field private OurActivity:Lcom/trendy/ddapp/ddapp;

.method public constructor <init>(Landroid/content/Context;)V
    .locals 1
    invoke-direct {p0, p1}, Landroid/widget/EditText;-><init>(Landroid/content/Context;)V
    check-cast p1, Lcom/trendy/ddapp/ddapp;
    iput-object p1, p0, Lcom/trendy/ddapp/EditTextBackEvent;->OurActivity:Lcom/trendy/ddapp/ddapp;
    const/4 v0, 0x1
    invoke-virtual {p0, v0}, Lcom/trendy/ddapp/EditTextBackEvent;->setSingleLine(Z)V
    const v0, 0x10000006    # IME_ACTION_DONE | IME_FLAG_NO_EXTRACT_UI
    invoke-virtual {p0, v0}, Lcom/trendy/ddapp/EditTextBackEvent;->setImeOptions(I)V
    invoke-virtual {p0, p0}, Lcom/trendy/ddapp/EditTextBackEvent;->setOnEditorActionListener(Landroid/widget/TextView$OnEditorActionListener;)V
    return-void
.end method

.method public final onEditorAction(Landroid/widget/TextView;ILandroid/view/KeyEvent;)Z
    .locals 2
    iget-object v0, p0, Lcom/trendy/ddapp/EditTextBackEvent;->OurActivity:Lcom/trendy/ddapp/ddapp;
    const/4 v1, 0x0
    invoke-virtual {v0, v1}, Lcom/trendy/ddapp/ddapp;->JavaCallback_HideKeyBoard(Z)V
    const/4 v0, 0x1
    return v0
.end method

.method public final onKeyPreIme(ILandroid/view/KeyEvent;)Z
    .locals 2
    const/4 v0, 0x4
    if-ne p1, v0, :not_back
    invoke-virtual {p2}, Landroid/view/KeyEvent;->getAction()I
    move-result v0
    const/4 v1, 0x1
    if-ne v0, v1, :consume
    iget-object v0, p0, Lcom/trendy/ddapp/EditTextBackEvent;->OurActivity:Lcom/trendy/ddapp/ddapp;
    const/4 v1, 0x0
    invoke-virtual {v0, v1}, Lcom/trendy/ddapp/ddapp;->JavaCallback_HideKeyBoard(Z)V
    :consume
    const/4 v0, 0x1
    return v0
    :not_back
    invoke-super {p0, p1, p2}, Landroid/widget/EditText;->onKeyPreIme(ILandroid/view/KeyEvent;)Z
    move-result v0
    return v0
.end method
