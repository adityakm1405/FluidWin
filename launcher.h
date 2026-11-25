// Launcher di FluidWin: tutte le app con la ricerca (tasto Win o icona "App" del dock). Metodi di dock::Motore,
// dichiarati in dock.h (che include questo file).
#pragma once

namespace dock {

inline std::wstring codificaQueryWeb(const std::wstring& query) {
    const int n=WideCharToMultiByte(CP_UTF8,0,query.c_str(),(int)query.size(),nullptr,0,nullptr,nullptr);
    if(n<=0) return L"";
    std::string utf8(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,query.c_str(),(int)query.size(),utf8.data(),n,nullptr,nullptr);
    const wchar_t* hex=L"0123456789ABCDEF";
    std::wstring out;
    for(unsigned char b:utf8) {
        if((b>='a'&&b<='z')||(b>='A'&&b<='Z')||(b>='0'&&b<='9')||b=='-'||b=='_'||b=='.'||b=='~') out+=(wchar_t)b;
        else { out+=L'%'; out+=hex[b>>4]; out+=hex[b&15]; }
    }
    return out;
}

// Preferiti del workspace salvati per utente; il file resta fuori dalla cartella del programma.
inline std::wstring percorsoConfigLauncher() {
    wchar_t base[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base)))
        return L"launcher.ini";
    const std::wstring dir = std::wstring(base) + L"\\FluidWin";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\launcher.ini";
}

// ---- launcher: tutte le app, con la ricerca (tasto Win o icona "App" del dock) ----

inline DWORD WINAPI Motore::threadIcone(LPVOID p) {
    Motore* m = (Motore*)p;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!m->fineIcone) {
        WaitForSingleObject(m->eventoIcone, INFINITE);
        while (!m->fineIcone) {
            std::pair<std::wstring, int> r;
            EnterCriticalSection(&m->csIcone);
            const bool c = !m->iconeDaFare.empty();
            if (c) { r = m->iconeDaFare.front(); m->iconeDaFare.pop_front(); }
            LeaveCriticalSection(&m->csIcone);
            if (!c) break;
            IconaPronta ip; ip.percorso = r.first; ip.lato = r.second;
            ip.ok = pixelIcona(r.first, r.second, ip.w, ip.h, ip.px);
            EnterCriticalSection(&m->csIcone); m->iconePronte.push_back(std::move(ip)); LeaveCriticalSection(&m->csIcone);
            PostMessageW(m->icone, WM_ICONA_PRONTA, 0, 0);
        }
    }
    CoUninitialize();
    return 0;
}

inline auto Motore::chiaveIcona(const std::wstring& percorso, int lato) -> std::wstring { return std::to_wstring(lato) + L"|" + percorso; }

inline DWORD WINAPI Motore::threadSottofondo(LPVOID p) {
    Sottofondo* l = (Sottofondo*)p;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l->fai();
    CoUninitialize();
    // Consegna non riuscita con la finestra ancora viva (memoria finita per un momento, coda piena): si riprova per
    // qualche secondo, perche' il completamento azzera lo stato "in corso" del launcher e perso bloccava le ricerche
    // successive (R20261121-122837-S3, R20261121-123050-S1). Finestra sparita: FluidWin si sta chiudendo.
    // Senza limite finche' la finestra vive: dopo 5 s si buttava il completamento, e la ricerca restava "in corso"
    // fino al riavvio (R20261121-140048-S5); dopo il primo secondo un tentativo ogni mezzo secondo
    for (int i = 0; !consegna::posta(l->dest, WM_LAVORO_FATTO, 0, l); i++) {
        if (!IsWindow(l->dest)) { delete l; break; }
        Sleep(i < 10 ? 100 : 500);
    }
    return 0;
}

inline auto Motore::inSottofondo(std::function<void()> fai, std::function<void()> applica) -> void {
    Sottofondo* l = new Sottofondo{std::move(fai), std::move(applica), icone};
    // nel pool di thread (vedi pool.h); se non parte, subito qui, come prima
    if (!pool::lancia(threadSottofondo, l, THREAD_PRIORITY_BELOW_NORMAL, true)) { l->fai(); l->applica(); delete l; }
}

inline auto Motore::chiediIcona(const std::wstring& percorso, int lato) -> void {
    if (percorso.empty() || !iconeChieste.insert(chiaveIcona(percorso, lato)).second) return;
    if (!csIconeOk) {
        InitializeCriticalSection(&csIcone); csIconeOk = true;
        eventoIcone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        threadIconeH = CreateThread(nullptr, 0, threadIcone, this, 0, nullptr);
        if (threadIconeH) SetThreadPriority(threadIconeH, THREAD_PRIORITY_BELOW_NORMAL);
    }
    EnterCriticalSection(&csIcone); iconeDaFare.push_back({percorso, lato}); LeaveCriticalSection(&csIcone);
    iconeAttese++;
    SetEvent(eventoIcone);
}

// all'uscita il thread si ferma (dopo l'icona che sta preparando) prima che il Motore sparisca
inline auto Motore::fermaIcone() -> void {
    if (!threadIconeH) return;
    fineIcone = 1; SetEvent(eventoIcone);
    WaitForSingleObject(threadIconeH, 2000);
    CloseHandle(threadIconeH); threadIconeH = nullptr;
}

inline auto Motore::sorgenteIcona(const Voce& v) -> std::wstring {
    if (v.avvio == AVVIO_PILA) return cartellaDownload();   // l'icona della cartella Download
    if (!v.pacchetto.empty()) return v.pacchetto;             // app di pacchetto fissata (vedi Voce::pacchetto)
    if (!v.ripiego.empty()) return v.ripiego;                 // Squirrel, versione cancellata (vedi Voce::ripiego)
    if (v.iconaRotta && !v.exe.empty() && GetFileAttributesW(v.exe.c_str()) != INVALID_FILE_ATTRIBUTES) return v.exe;   // (vedi Voce::iconaRotta)
    return v.fissata ? v.avvio : (v.avvio.rfind(L"shell:AppsFolder\\", 0) == 0 ? v.avvio : v.exe);
}

// le icone arrivate dal thread diventano bitmap (veloce) e vanno a chi le aspetta: true se ne e' arrivata qualcuna
inline auto Motore::raccogliIcone() -> bool {
    if (!csIconeOk) return false;
    std::vector<IconaPronta> arrivate;
    EnterCriticalSection(&csIcone); arrivate.swap(iconePronte); LeaveCriticalSection(&csIcone);
    for (auto& ip : arrivate) {
        iconeAttese--;
        ID2D1Bitmap1* b = ip.ok ? bitmapDaPixel(ip.w, ip.h, ip.px) : nullptr;
        bool presa = false;
        if (ip.lato == 256) {          // dock
            for (auto& v : voci) if (!v.icona && v.avvio != AVVIO_LAUNCHER && v.avvio != AVVIO_CONTROLLO && sorgenteIcona(v) == ip.percorso) { v.icona = b; presa = true; dirty = true; break; }
            // (si potra' richiedere: l'app puo' chiudersi e riaprirsi; se e' fallita si riprova al prossimo aggiornamento)
            iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
        } else if (ip.lato == 128) {   // app in background (se non c'e', la piccola foto salvata da Windows)
            if (!b) b = (ID2D1Bitmap1*)fotoIconaSfondo(ip.percorso);
            cacheIcone[ip.percorso] = b; presa = b != nullptr;
            for (auto& a : appSfondo) if (a.exe == ip.percorso) a.icona = b;
            pDirty = true; altra.pDirty = true;
        } else if (ip.lato == 96) {    // launcher
            for (auto& a : tutteApp) if (a.avvio == ip.percorso && !a.icona) { a.icona = b; presa = true; lDirty = true; break; }
        } else if (ip.lato == 48) {    // file e cartelle del launcher (anche "nessuna": non si richiede a ogni ricerca)
            auto& v = iconeFile[ip.percorso];
            if (!v) { v = b; presa = b != nullptr; }
            iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
            lDirty = true;
        } else if (ip.lato == 64) {    // notifiche
            const std::wstring pre = L"shell:AppsFolder\\";
            if (ip.percorso.rfind(pre, 0) == 0) { iconeN[ip.percorso.substr(pre.size())] = b; presa = true; }
            else if (auto it = iconeNDaExe.find(ip.percorso); it != iconeNDaExe.end()) { iconeN[it->second] = b; presa = true; }
            pDirty = true; altra.pDirty = true;
        }
        if (b && !presa) b->Release();
    }
    return !arrivate.empty();
}

inline auto Motore::launcherDaDisegnare() const -> bool { return lAperto && (lDirty || lAnim); }

inline auto Motore::creaLauncher(HINSTANCE inst) -> bool {
    LW = std::ceil(740 * dpi); LH = LHMAX = std::ceil(574 * dpi); LEXTRA = std::ceil(160 * dpi);
    WNDCLASSW wl{}; wl.lpfnWndProc = procLauncher; wl.hInstance = inst; wl.lpszClassName = L"FluidWin.Launcher";
    wl.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wl);
    launcher = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wl.lpszClassName, L"App",
                               WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    vetroLauncher = creaVetro(inst);
    BOOL niente = TRUE; DwmSetWindowAttribute(launcher, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
    // ogni passo puo' fallire: false (niente launcher), mai un puntatore nullo usato dopo (audit FW-AUDIT-004)
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fab = nullptr;
    HRESULT hr = dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    if (SUCCEEDED(hr)) hr = dd->GetAdapter(&ad);
    if (SUCCEEDED(hr)) hr = ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fab);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)LW; sd.Height = (UINT)(LH + LEXTRA); sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &scL);
    }
    if (fab) fab->Release();
    if (ad) ad->Release();
    if (dd) dd->Release();
    if (FAILED(hr)) return false;
    if (FAILED(dcomp->CreateTargetForHwnd(launcher, TRUE, &targetL)) || FAILED(dcomp->CreateVisual(&visualL)) ||
        FAILED(dcomp->CreateEffectGroup(&effettoL))) return false;
    visualL->SetContent(scL); visualL->SetEffect(effettoL);
    targetL->SetRoot(visualL); dcomp->Commit();
    IDXGISurface* sup = nullptr;
    if (FAILED(scL->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup)) || !sup) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglioL); sup->Release();
    if(bersaglioL) {
        if(!lPinContorno) lPinContorno=caricaIconaSvgRisorsa(210,64);
        if(!lPinPieno) lPinPieno=caricaIconaSvgRisorsa(211,64);
    }
    return bersaglioL != nullptr;
}

// solo le app vere: niente disinstallazioni, documenti, pagine web e guide
inline auto Motore::appUtile(const std::wstring& nome, const std::wstring& par) -> bool {
    const std::wstring n = minuscolo(nome), p = minuscolo(par);
    if (n.find(L"uninstall") != std::wstring::npos || n.find(L"disinstall") != std::wstring::npos) return false;
    if (p.rfind(L"http", 0) == 0) return false;
    const size_t barra = p.find_last_of(L"\\/"), punto = p.rfind(L'.');
    if (barra == std::wstring::npos) return true;   // AppUserModelID (app dello Store e molte app desktop)
    if (punto == std::wstring::npos || punto < barra) return true;
    const std::wstring est = p.substr(punto + 1);
    return est == L"exe" || est == L"msc" || est == L"lnk" || est == L"cpl";
}

inline auto Motore::leggiTutteApp(bool subito) -> void {
    lLetto = adesso();
    if (subito) { Cronometro cr(this, "leggiTutteApp"); applicaTutteApp(elencaTutteApp()); return; }
    if (lavoroTutte) return;
    lavoroTutte = true;
    auto ris = std::make_shared<std::vector<AppL>>();
    inSottofondo([ris] { *ris = elencaTutteApp(); }, [this, ris] { lavoroTutte = false; applicaTutteApp(std::move(*ris)); });
}

inline auto Motore::applicaTutteApp(std::vector<AppL> nuove) -> void {
    // le icone gia' caricate si conservano
    for (auto& a : nuove) for (auto& v : tutteApp) if (v.avvio == a.avvio) { a.icona = v.icona; a.provata = v.provata; v.icona = nullptr; break; }
    for (auto& v : tutteApp) if (v.icona) v.icona->Release();
    tutteApp = std::move(nuove);
    aggiornaPinned();
    inizializzaCartelle();
    // launcher aperto (l'elenco nuovo arriva mentre si guarda): la scelta e lo scorrimento restano dove sono
    const int sel = lSel; const double scr = lScrollObj;
    filtra();
    if (lAperto) { lSel = std::clamp(sel, 0, std::max(0, (int)filtrate.size() - 1)); lScrollObj = std::clamp(scr, 0.0, lScrollMax()); }
    for (auto& a : tutteApp) if (!a.icona && !a.provata) { a.provata = true; chiediIcona(a.avvio, 96); }
    if (registro) { char b[64]; snprintf(b, 64, "launcher: %d app", (int)tutteApp.size()); scrivi(b); }
}

inline auto Motore::elencaTutteApp() -> std::vector<AppL> {
    std::vector<AppL> nuove;
    IShellItem* cart = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&cart)))) return nuove;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(cart->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK) {
            LPWSTR n = nullptr, par = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &par)) &&
                n && par && n[0] && appUtile(n, par)) {
                AppL a; a.nome = n; a.avvio = std::wstring(L"shell:AppsFolder\\") + par; a.minuscolo = minuscolo(n);
                nuove.push_back(std::move(a));
            }
            CoTaskMemFree(n); CoTaskMemFree(par); it->Release();
        }
        en->Release();
    }
    cart->Release();
    std::sort(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) { return lstrcmpiW(a.nome.c_str(), b.nome.c_str()) < 0; });
    nuove.erase(std::unique(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) { return a.minuscolo == b.minuscolo; }), nuove.end());
    return nuove;
}

// con la ricerca: prima i nomi che iniziano con il testo, poi quelli che lo contengono
// Ricerca: prima i nomi che iniziano col testo (o una parola che inizia col testo), poi le iniziali delle parole
// ("vsc" -> Visual Studio Code), poi i nomi che lo contengono, infine quelli che ne contengono le lettere in
// ordine ("clcl" -> Calcolatrice)
inline auto Motore::iniziali(const std::wstring& nome, const std::wstring& q) -> bool {
    size_t k = 0;
    for (size_t i = 0; i < nome.size() && k < q.size(); i++)
        if ((i == 0 || nome[i - 1] == L' ' || nome[i - 1] == L'-') && nome[i] == q[k]) k++;
    return k == q.size();
}

inline auto Motore::sottosequenza(const std::wstring& nome, const std::wstring& q) -> bool {
    size_t k = 0;
    for (size_t i = 0; i < nome.size() && k < q.size(); i++) if (nome[i] == q[k]) k++;
    return k == q.size();
}

inline auto Motore::filtra() -> void {
    filtrate.clear();
    const std::wstring q = minuscolo(query);
    std::vector<int> livelli[3];   // iniziali, contiene, lettere in ordine
    for (int i = 0; i < (int)tutteApp.size(); i++) {
        if (q.empty()) { filtrate.push_back(i); continue; }
        const std::wstring& n = tutteApp[i].minuscolo;
        const size_t p = n.find(q);
        if (p == 0 || (p != std::wstring::npos && n[p - 1] == L' ')) filtrate.push_back(i);
        else if (q.size() >= 2 && iniziali(n, q)) livelli[0].push_back(i);
        else if (p != std::wstring::npos) livelli[1].push_back(i);
        else if (q.size() >= 3 && sottosequenza(n, q)) livelli[2].push_back(i);
    }
    for (auto& l : livelli) filtrate.insert(filtrate.end(), l.begin(), l.end());
    lSel = 0; lScrollObj = 0; lDirty = true; lSelTastiera = false;
    // file e cartelle: testo nuovo -> ricerca 0,2 s dopo l'ultimo tasto (una per parola, non per lettera); testo vuoto
    // -> via i risultati e scartata una ricerca in corso. (filtra si chiama anche quando arriva l'elenco nuovo delle
    // app: col testo uguale non si ricerca)
    lSelFile = -1; lSopraFile = -1;
    if (query.empty()) { lFile.clear(); lFileTesto.clear(); lFileGen++; if (launcher) KillTimer(launcher, TIMER_FILE_L); }
    else if (query != lFileTesto && launcher) SetTimer(launcher, TIMER_FILE_L, 200, nullptr);
}

// Preferiti nella parte superiore: ogni voce viene risolta tra le app installate reali.
inline auto Motore::aggiornaPinned() -> void {
    const std::vector<std::vector<std::wstring>> preferiti = {
        {L"visual studio code", L"vs code"},
        {L"figma"},
        {L"linear"},
        {L"docker"},
        {L"raycast"},
        {L"notion"}
    };
    // Il marcatore distingue la prima esecuzione da una lista salvata intenzionalmente vuota.
    wchar_t salvati[8192] = L"";
    const std::wstring ini = percorsoConfigLauncher();
    GetPrivateProfileStringW(L"Workspace",L"Pinned",L"__DEFAULT__",salvati,(DWORD)std::size(salvati),ini.c_str());
    lPinnedManual.clear();
    if (wcscmp(salvati,L"__DEFAULT__")==0) {
        for (const auto& gruppo : preferiti) {
            for (const auto& app : tutteApp) {
                const std::wstring& n=app.minuscolo;
                if (std::any_of(gruppo.begin(),gruppo.end(),[&](const std::wstring& s) {
                        return n==s || n.find(s)!=std::wstring::npos;
                    })) {
                    lPinnedManual.push_back(app.avvio);
                    break;
                }
            }
        }
        salvaPinned();
    } else {
        std::wstring elenco=salvati;
        size_t inizio=0;
        while(inizio<elenco.size()) {
            const size_t fine=elenco.find(L'|',inizio);
            const std::wstring percorso=elenco.substr(inizio,fine==std::wstring::npos?std::wstring::npos:fine-inizio);
            if(!percorso.empty()) lPinnedManual.push_back(percorso);
            if(fine==std::wstring::npos) break;
            inizio=fine+1;
        }
    }
    // Hard limit: keep the workspace compact and always leave room for Instant Actions.
    if(lPinnedManual.size()>6) {
        lPinnedManual.resize(6);
        salvaPinned();
    }
    lPinned.clear();
    for(const auto& percorso:lPinnedManual) {
        for(int i=0;i<(int)tutteApp.size();++i) {
            if(tutteApp[i].avvio==percorso) { lPinned.push_back(i); break; }
        }
    }
}
inline auto Motore::salvaPinned() -> void {
    std::wstring elenco;
    for(const auto& percorso:lPinnedManual) {
        if(!elenco.empty()) elenco+=L"|";
        elenco+=percorso;
    }
    const std::wstring ini=percorsoConfigLauncher();
    WritePrivateProfileStringW(L"Workspace",L"Pinned",elenco.c_str(),ini.c_str());
}
inline auto Motore::togglePinned(int indiceApp) -> void {
    if(indiceApp<0 || indiceApp>=(int)tutteApp.size()) return;
    const std::wstring& percorso=tutteApp[indiceApp].avvio;
    auto it=std::find(lPinnedManual.begin(),lPinnedManual.end(),percorso);
    if(it!=lPinnedManual.end()) lPinnedManual.erase(it);
    else {
        if(lPinnedManual.size()>=6) {
            MessageBoxW(launcher,testi::t(L"Puoi fissare al massimo 6 app nel Workspace.",L"You can pin a maximum of 6 apps in the Workspace."),
                L"FluidWin",MB_OK|MB_ICONINFORMATION);
            return;
        }
        lPinnedManual.push_back(percorso);
    }
    salvaPinned();
    aggiornaPinned();
    lDirty=true;
}

// ---- file e cartelle sotto la griglia (issue #14) ----

// Righe di file che entrano (fino a LF_MAX), lasciando alla griglia almeno una riga di app; -1 senza testo (niente
// sezione). L'indice LF_ESPLORA della riga "Cerca in Esplora file" e' questo stesso numero
inline auto Motore::lFileRighe() const -> int {
    if (query.empty()) return -1;
    const float spazio = LH - 14 * dpi - lGrigliaTop() - lCellaH() - (LF_TESTA + LF_ESPLORA) * dpi;
    const int entrano = std::max(0, (int)std::floor(spazio / (LF_RIGA * dpi)));
    return std::min({(int)lFile.size(), LF_MAX, entrano});
}

// altezza a cui va la sezione (lFileH la insegue in renderLauncher)
inline auto Motore::lFileAlto() const -> float {
    const int r = lFileRighe();
    return r < 0 ? 0.0f : (LF_TESTA + r * LF_RIGA + LF_ESPLORA) * dpi;
}

// riga della sezione sotto il punto: 0..righe-1 i file, righe = "Cerca in Esplora file", -1 fuori
inline auto Motore::lFileIn(float x, float y) const -> int {
    const int r = lFileRighe();
    if (r < 0 || x < 16 * dpi || x > LW - 16 * dpi) return -1;
    const float top = LH - 14 * dpi - (float)lFileH, fondo = LH - 14 * dpi;
    if (y < top || y >= fondo) return -1;
    const float y0 = top + LF_TESTA * dpi;
    if (y < y0) return -1;
    const int k = (int)((y - y0) / (LF_RIGA * dpi));
    if (k < r) return lFileTesto == query ? k : -1;   // (righe di un testo vecchio: si vedono sbiadite, non si toccano)
    const float ye = y0 + r * LF_RIGA * dpi;
    return y >= ye && y < ye + LF_ESPLORA * dpi ? r : -1;
}

// la ricerca nel thread; il risultato si usa solo se e' ancora del testo scritto (generazione e testo uguali)
// Una ricerca alla volta: con Windows Search lento le ricerche vecchie si accumulavano nel pool. Arrivata la risposta,
// se il testo e' cambiato intanto ne parte una sola col testo di adesso (R20261121-002818-S10)
inline auto Motore::cercaFileLauncher() -> void {
    if (query.empty() || !lAperto) return;
    if (lFileInCorso) { lFileAttesa = true; return; }
    const std::wstring q = query;
    const unsigned g = ++lFileGen;
    auto ris = std::make_shared<std::vector<indice::File>>();
    lFileInCorso = true; lFileAttesa = false;
    inSottofondo([q, ris] { *ris = indice::cerca(q, LF_MAX, true); },
                 [this, q, g, ris] {
                     lFileInCorso = false;
                     // (anche a testo uguale se la generazione e' cambiata: svuotato e riscritto lo stesso testo, la
                     // risposta vecchia si scarta e senza nuova ricerca l'elenco restava vuoto, R20261121-115814-S1)
                     if (lFileAttesa) { lFileAttesa = false; if (lAperto && !lChiusura && !query.empty() && (q != query || g != lFileGen)) { cercaFileLauncher(); return; } }
                     if (g != lFileGen || !lAperto || lChiusura || q != query) return;
                     // "Cerca in Esplora file" scelto con le frecce resta lui anche se arrivano file (stava all'indice
                     // delle righe, e il primo file prendeva il suo posto: Invio apriva il file, R20261121-002818-S8)
                     const bool esplora = lSelFile >= 0 && lSelFile == lFileRighe();
                     lFile = std::move(*ris); lFileTesto = q;
                     if (esplora) lSelFile = lFileRighe();
                     // (le icone di file gia' viste restano; oltre 200 la cache si svuota)
                     if (iconeFile.size() > 200) { for (auto& [p, b] : iconeFile) if (b) b->Release(); iconeFile.clear(); }
                     for (const auto& f : lFile) if (!iconeFile.count(f.percorso)) chiediIcona(f.percorso, 48);
                     lSopraFile = -1; lSelFile = std::min(lSelFile, lFileRighe());
                     lDirty = true;
                 });
}

// apre il file (o la cartella) k, oppure la ricerca completa in Esplora file (k = righe)
inline auto Motore::apriFile(int k) -> void {
    const int r = lFileRighe();
    if (r < 0 || k < 0 || k > r) return;
    if (k == r) {
        const std::wstring cerca = indice::cercaInEsplora(query);
        chiudiLauncher();
        ShellExecuteW(nullptr, L"open", cerca.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        scrivi("launcher: ricerca in Esplora file");
        return;
    }
    // (solo risultati del testo scritto: R20261119-195000-S2, prima "fattura" -> "contratto", Giu' e Invio prima dei
    // risultati nuovi apriva la fattura)
    if (k >= (int)lFile.size() || lFileTesto != query) return;
    const std::wstring p = lFile[k].percorso;
    chiudiLauncher();
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    scrivi("launcher: file aperto");   // (il percorso no: e' dell'utente)
}

inline auto Motore::menuFile(int k, POINT pt) -> void {
    if (k < 0 || k >= (int)lFile.size() || k >= lFileRighe() || lFileTesto != query) return;
    const indice::File f = lFile[k];   // (copia: il menu ha un suo ciclo dei messaggi, l'elenco puo' cambiare)
    const int s = (int)menu::mostra({menu::titolo(f.nome), menu::voce(testi::t(L"Apri", L"Open"), 1),
                                     menu::voce(testi::t(L"Apri percorso file", L"Open file location"), 2)}, pt, launcher);
    if (s == 1) { chiudiLauncher(); ShellExecuteW(nullptr, L"open", f.percorso.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    else if (s == 2) { chiudiLauncher(); ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + f.percorso + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
}

// true se un'animazione della sezione e' in corso (il launcher deve ridisegnarsi)
inline auto Motore::disegnaFileLauncher(bool scuro, D2D1_COLOR_F ink) -> bool {
    bool muove = false;
    const int r = lFileRighe();
    if (lFileH < 1) return false;
    const float top = LH - 14 * dpi - (float)lFileH, fondo = LH - 14 * dpi;
    const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.6f};
    const D2D1_COLOR_F blu = scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1);
    dc->PushAxisAlignedClip({0, top, LW, fondo}, D2D1_ANTIALIAS_MODE_ALIASED);
    // linea sopra, titolo
    pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.08f));
    dc->DrawLine({24 * dpi, top + 0.5f}, {LW - 24 * dpi, top + 0.5f}, pennello, 1.0f);
    pennello->SetColor(tenue);
    scrivi(testi::t(L"File e cartelle", L"Files and folders"), fmtSotto, {28 * dpi, top + 6 * dpi, LW - 28 * dpi, top + LF_TESTA * dpi});
    auto evidenziaRiga = [&](const D2D1_RECT_F& rr, int k) {
        const bool sopra = k == lSopraFile, scelta = k == lSelFile;
        if (!sopra && !scelta) return;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, sopra ? 0.12f : 0.08f) : D2D1::ColorF(1, 1, 1, sopra ? 0.55f : 0.4f));
        dc->FillRoundedRectangle({rr, 8 * dpi, 8 * dpi}, pennello);
        if (scelta) { pennello->SetColor(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.7f)); dc->DrawRoundedRectangle({rr, 8 * dpi, 8 * dpi}, pennello, 1.5f * dpi); }
    };
    float y = top + LF_TESTA * dpi;
    // Righe di un testo vecchio (si sta scrivendo, i risultati nuovi non sono arrivati): restano al loro posto, cosi'
    // la sezione non salta a ogni lettera, ma sbiadiscono e non si possono scegliere (R20261119-195000-S2). L'opacita'
    // va verso il suo valore con dolcezza (lFileOp)
    {
        const double obj = lFileTesto == query ? 1.0 : 0.45;
        lFileOp += (obj - lFileOp) * (1 - std::exp(-dtFotogramma / 0.06));
        if (std::abs(lFileOp - obj) < 0.004) lFileOp = obj; else muove = true;
    }
    if (lFileOp < 0.999) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), (float)lFileOp), nullptr);
    for (int k = 0; k < r && k < (int)lFile.size(); k++) {
        const indice::File& f = lFile[k];
        const D2D1_RECT_F rr = {20 * dpi, y + 2 * dpi, LW - 20 * dpi, y + (LF_RIGA - 2) * dpi};
        evidenziaRiga(rr, k);
        const float cy = y + LF_RIGA * dpi / 2, ix = 30 * dpi;
        const D2D1_RECT_F ri = {ix, cy - 14 * dpi, ix + 28 * dpi, cy + 14 * dpi};
        auto it = iconeFile.find(f.percorso);
        if (it != iconeFile.end() && it->second) dc->DrawBitmap(it->second, ri, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else { pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.35f)); dc->FillRoundedRectangle({ri, 5 * dpi, 5 * dpi}, pennello); }   // (icona in arrivo)
        pennello->SetColor(ink);
        scrivi(f.nome, fmtVoce, {ix + 38 * dpi, y + 4 * dpi, LW - 32 * dpi, y + 24 * dpi});
        pennello->SetColor(tenue);
        scrivi(f.cartella, fmtSotto, {ix + 38 * dpi, y + 23 * dpi, LW - 32 * dpi, y + 40 * dpi});
        y += LF_RIGA * dpi;
    }
    if (lFileOp < 0.999) dc->PopLayer();
    // "Cerca «testo» in Esplora file": la ricerca completa di Windows
    {
        const D2D1_RECT_F rr = {20 * dpi, y + 2 * dpi, LW - 20 * dpi, y + (LF_ESPLORA - 2) * dpi};
        evidenziaRiga(rr, r);
        const float cy = y + LF_ESPLORA * dpi / 2, lx = 44 * dpi;
        pennello->SetColor(blu);
        dc->DrawEllipse({{lx - 2 * dpi, cy - 2 * dpi}, 6 * dpi, 6 * dpi}, pennello, 1.8f * dpi);
        linea(2.5f, 2.5f, 7, 7, lx, cy);
        const std::wstring t = testi::t(L"Cerca “", L"Search “") + testi::isola(query) + testi::t(L"” in Esplora file", L"” in File Explorer");
        scrivi(t, fmtVoce, {lx + 24 * dpi, y, LW - 32 * dpi, y + LF_ESPLORA * dpi});
    }
    dc->PopAxisAlignedClip();
    return muove;
}

// Misure e coordinate della nuova area di lavoro, ancorata al dock.
inline auto Motore::lDividerX() const -> float { return LW * 0.48f; }
inline auto Motore::lFooterTop() const -> float { return LH - 52 * dpi; }

inline auto Motore::lCategoriaPredefinita(const AppL& app) const -> int {
    const std::wstring& n = app.minuscolo;
    auto contiene = [&](std::initializer_list<const wchar_t*> parole) {
        for (const wchar_t* p : parole) if (n.find(p) != std::wstring::npos) return true;
        return false;
    };
    if (contiene({L"visual studio",L"vs code",L"code",L"unity",L"docker",L"git",L"postman",L"tableplus",L"neovim",L"pycharm",L"intellij",L"android studio",L"node",L"python",L"raycast",L"terminal"})) return 0;
    if (contiene({L"steam",L"epic games",L"ubisoft",L"ea app",L"battle.net",L"gog galaxy",L"xbox",L"riot client",L"minecraft",L"playnite",L"game",L"discord",L"lutris",L"moonlight",L"retroarch",L"mangohud"})) return 1;
    if (contiene({L"figma",L"blender",L"photoshop",L"illustrator",L"premiere",L"after effects",L"gimp",L"krita",L"paint.net",L"affinity",L"obs studio",L"davinci",L"audacity",L"darktable"})) return 2;
    if (contiene({L"chrome",L"firefox",L"edge",L"brave",L"opera",L"slack",L"zoom",L"teams",L"telegram",L"whatsapp",L"thunderbird",L"arc browser"})) return 3;
    if (contiene({L"word",L"excel",L"powerpoint",L"onenote",L"notion",L"linear",L"obsidian",L"libreoffice",L"outlook",L"office",L"todo",L"calendar",L"calculator"})) return 4;
    if (contiene({L"settings",L"control panel",L"task manager",L"powershell",L"command prompt",L"windows tools",L"file explorer",L"security",L"defender",L"system"})) return 5;
    return 4;
}

inline auto Motore::lIndiceCartella(const std::wstring& avvio) const -> int {
    for (int k=0;k<(int)lCartelle.size();++k)
        if (std::find(lCartelle[k].app.begin(),lCartelle[k].app.end(),avvio)!=lCartelle[k].app.end()) return k;
    return -1;
}
inline auto Motore::lCategoriaApp(const AppL& app) const -> int { return lIndiceCartella(app.avvio); }
inline auto Motore::lNomeCategoria(const AppL& app) const -> std::wstring {
    const int k=lCategoriaApp(app);
    return k>=0 && k<(int)lCartelle.size() ? lCartelle[k].nome : testi::t(L"Non assegnata",L"Unassigned");
}
inline auto Motore::lAppCategoria(int categoria) const -> std::vector<int> {
    std::vector<int> app;
    if(categoria<0 || categoria>=(int)lCartelle.size()) return app;
    for(int i=0;i<(int)tutteApp.size();++i)
        if(std::find(lCartelle[categoria].app.begin(),lCartelle[categoria].app.end(),tutteApp[i].avvio)!=lCartelle[categoria].app.end())
            app.push_back(i);
    return app;
}

inline auto Motore::salvaCartelle() -> void {
    const std::wstring ini=percorsoConfigLauncher();
    WritePrivateProfileStringW(L"Library",L"FolderCount",std::to_wstring(lCartelle.size()).c_str(),ini.c_str());
    for(int i=0;i<(int)lCartelle.size();++i) {
        const std::wstring sezione=L"Folder"+std::to_wstring(i);
        const CartellaL& f=lCartelle[i];
        WritePrivateProfileStringW(sezione.c_str(),L"Name",f.nome.c_str(),ini.c_str());
        WritePrivateProfileStringW(sezione.c_str(),L"Icon",std::to_wstring(f.icona).c_str(),ini.c_str());
        WritePrivateProfileStringW(sezione.c_str(),L"Color",std::to_wstring(f.colore).c_str(),ini.c_str());
        WritePrivateProfileStringW(sezione.c_str(),L"Custom",f.personalizzata?L"1":L"0",ini.c_str());
        std::wstring elenco;
        for(const auto& p:f.app) { if(!elenco.empty()) elenco+=L"|"; elenco+=p; }
        WritePrivateProfileStringW(sezione.c_str(),L"Apps",elenco.c_str(),ini.c_str());
    }
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,ini.c_str());
}
inline auto Motore::caricaCartelle() -> void {
    lCartelle.clear();
    const std::wstring ini=percorsoConfigLauncher();
    wchar_t conteggio[32]=L"";
    GetPrivateProfileStringW(L"Library",L"FolderCount",L"",conteggio,(DWORD)std::size(conteggio),ini.c_str());
    if(!conteggio[0]) {
        const wchar_t* nomi[6]={L"Development",L"Gaming",L"Creative",L"Web & Comms",L"Productivity",L"System"};
        for(int k=0;k<6;++k) {
            CartellaL f; f.nome=nomi[k]; f.icona=k; f.colore=k; f.personalizzata=false;
            for(const auto& app:tutteApp) if(lCategoriaPredefinita(app)==k) f.app.push_back(app.avvio);
            lCartelle.push_back(std::move(f));
        }
        salvaCartelle();
        WritePrivateProfileStringW(L"Library",L"CustomFoldersResetV1",L"1",ini.c_str());
        return;
    }
    const int n=std::clamp(_wtoi(conteggio),0,32);
    const bool puliziaUnaVolta=GetPrivateProfileIntW(L"Library",L"CustomFoldersResetV1",0,ini.c_str())==0;
    for(int i=0;i<n;++i) {
        const std::wstring sezione=L"Folder"+std::to_wstring(i);
        wchar_t nome[128]=L"",apps[32768]=L"";
        GetPrivateProfileStringW(sezione.c_str(),L"Name",L"",nome,(DWORD)std::size(nome),ini.c_str());
        if(!nome[0]) continue;
        GetPrivateProfileStringW(sezione.c_str(),L"Apps",L"",apps,(DWORD)std::size(apps),ini.c_str());
        CartellaL f; f.nome=nome;
        f.icona=std::clamp((int)GetPrivateProfileIntW(sezione.c_str(),L"Icon",i%6,ini.c_str()),0,5);
        f.colore=std::clamp((int)GetPrivateProfileIntW(sezione.c_str(),L"Color",f.icona,ini.c_str()),0,5);
        f.personalizzata=GetPrivateProfileIntW(sezione.c_str(),L"Custom",0,ini.c_str())!=0;
        std::wstring elenco=apps; size_t inizio=0;
        while(inizio<elenco.size()) {
            const size_t fine=elenco.find(L'|',inizio);
            std::wstring p=elenco.substr(inizio,fine==std::wstring::npos?std::wstring::npos:fine-inizio);
            if(!p.empty()) f.app.push_back(std::move(p));
            if(fine==std::wstring::npos) break;
            inizio=fine+1;
        }
        if(puliziaUnaVolta && f.personalizzata) continue;
        lCartelle.push_back(std::move(f));
    }
    if(puliziaUnaVolta) {
        salvaCartelle();
        WritePrivateProfileStringW(L"Library",L"CustomFoldersResetV1",L"1",ini.c_str());
    }
}
inline auto Motore::inizializzaCartelle() -> void {
    caricaCartelle();
    lAngoliCartella.resize(lCartelle.size(),0.0);
    if(lCartellaAperta>=(int)lCartelle.size()) lCartellaAperta=lCartelle.empty()?-1:0;
}
inline auto Motore::toggleAppCartella(int indiceApp, int cartella) -> void {
    if(indiceApp<0 || indiceApp>=(int)tutteApp.size() || cartella<0 || cartella>=(int)lCartelle.size()) return;
    const std::wstring& percorso=tutteApp[indiceApp].avvio;
    const bool eraInQuesta=std::find(lCartelle[cartella].app.begin(),lCartelle[cartella].app.end(),percorso)!=lCartelle[cartella].app.end();
    for(auto& f:lCartelle) f.app.erase(std::remove(f.app.begin(),f.app.end(),percorso),f.app.end());
    if(!eraInQuesta) lCartelle[cartella].app.push_back(percorso);
    salvaCartelle();
    lDirty=true;
}
inline auto Motore::rimuoviAppDaCartella(int indiceApp, int cartella) -> void {
    if(indiceApp<0 || indiceApp>=(int)tutteApp.size() || cartella<0 || cartella>=(int)lCartelle.size()) return;
    const std::wstring& percorso=tutteApp[indiceApp].avvio;
    auto& elenco=lCartelle[cartella].app;
    const size_t prima=elenco.size();
    elenco.erase(std::remove(elenco.begin(),elenco.end(),percorso),elenco.end());
    if(elenco.size()!=prima) {
        salvaCartelle();
        lSopraAppCartella=-1;
        lSopraRimuoviAppCartella=-1;
        lDirty=true;
    }
}
inline auto Motore::apriGestisciCartella(int cartella) -> void {
    if(cartella<0 || cartella>=(int)lCartelle.size()) return;
    lCartellaGestita=cartella; lModalita=2; lScrollEditor=0; lNomeNuovoAttivo=false; lDirty=true;
}
inline auto Motore::creaCartella() -> void {
    std::wstring nome=lNomeNuovaCartella;
    while(!nome.empty() && iswspace(nome.front())) nome.erase(nome.begin());
    while(!nome.empty() && iswspace(nome.back())) nome.pop_back();
    if(nome.empty()) { MessageBoxW(launcher,testi::t(L"Inserisci un nome per la cartella.",L"Enter a name for the folder."),L"FluidWin",MB_OK|MB_ICONINFORMATION); return; }
    if(lCartelle.size()>=32) { MessageBoxW(launcher,testi::t(L"Puoi creare fino a 32 cartelle.",L"You can create up to 32 folders."),L"FluidWin",MB_OK|MB_ICONINFORMATION); return; }
    for(const auto& f:lCartelle) if(!_wcsicmp(f.nome.c_str(),nome.c_str())) {
        MessageBoxW(launcher,testi::t(L"Esiste gia' una cartella con questo nome.",L"A folder with this name already exists."),L"FluidWin",MB_OK|MB_ICONINFORMATION);
        return;
    }
    CartellaL nuova; nuova.nome=nome; nuova.icona=0; nuova.colore=0; nuova.personalizzata=true;
    nuova.app=lAppNuovaCartella;
    for(auto& f:lCartelle) for(const auto& p:nuova.app)
        f.app.erase(std::remove(f.app.begin(),f.app.end(),p),f.app.end());
    lCartelle.push_back(std::move(nuova));
    lAngoliCartella.push_back(0.0);
    lCartellaAperta=(int)lCartelle.size()-1;
    salvaCartelle();
    lModalita=0; lNomeNuovaCartella.clear(); lAppNuovaCartella.clear(); lScrollEditor=0;
    lNomeNuovoAttivo=false; lDirty=true;
}
inline auto Motore::apriRinominaCartella(int cartella) -> void {
    if(cartella<0 || cartella>=(int)lCartelle.size()) return;
    lCartellaRinomina=cartella;
    lNomeNuovaCartella=lCartelle[cartella].nome;
    lNomeNuovoAttivo=true;
    lModalita=5; lScrollEditor=0; lDirty=true;
}
inline auto Motore::salvaRinominaCartella() -> void {
    if(lCartellaRinomina<0 || lCartellaRinomina>=(int)lCartelle.size()) return;
    std::wstring nome=lNomeNuovaCartella;
    while(!nome.empty() && iswspace(nome.front())) nome.erase(nome.begin());
    while(!nome.empty() && iswspace(nome.back())) nome.pop_back();
    if(nome.empty()) return;
    for(int i=0;i<(int)lCartelle.size();++i)
        if(i!=lCartellaRinomina && !_wcsicmp(lCartelle[i].nome.c_str(),nome.c_str())) {
            MessageBoxW(launcher,testi::t(L"Esiste gia' una cartella con questo nome.",L"A folder with this name already exists."),
                        L"FluidWin",MB_OK|MB_ICONINFORMATION);
            return;
        }
    lCartelle[lCartellaRinomina].nome=nome;
    salvaCartelle();
    lCartellaAperta=lCartellaRinomina;
    lCartellaRinomina=-1; lNomeNuovaCartella.clear(); lNomeNuovoAttivo=false;
    lModalita=0; lDirty=true;
}
inline auto Motore::rimuoviCartella(int cartella) -> void {
    if(cartella<0 || cartella>=(int)lCartelle.size()) return;
    lCartelle.erase(lCartelle.begin()+cartella);
    if(cartella<(int)lAngoliCartella.size()) lAngoliCartella.erase(lAngoliCartella.begin()+cartella);
    if(lCartellaAperta==cartella) lCartellaAperta=-1;
    else if(lCartellaAperta>cartella) --lCartellaAperta;
    lCartellaGestita=-1; lModalita=0; lScrollCartelle=0;
    salvaCartelle(); lDirty=true;
}
inline auto Motore::apriMenuCartella(int cartella, POINT pt) -> void {
    if(cartella<0 || cartella>=(int)lCartelle.size()) return;
    // Keep the context menu inside the rounded launcher surface.
    ScreenToClient(launcher,&pt);
    const float w=184*dpi,h=132*dpi;
    lCartellaMenuX=std::clamp((float)pt.x,10*dpi,std::max(10*dpi,LW-w-10*dpi));
    lCartellaMenuY=std::clamp((float)pt.y,64*dpi,std::max(64*dpi,LH-h-10*dpi));
    lCartellaMenuIndice=cartella;
    lCartellaMenuHover=-1;
    lCartellaMenuAperto=true;
    lDirty=true;
}
inline auto Motore::lCartellaY(int cartella) const -> float {
    float y=122*dpi-lScrollCartelle;
    for(int i=0;i<cartella && i<(int)lCartelle.size();++i)
        y+=(i==lCartellaAperta?114:36)*dpi+7*dpi;
    return y;
}
inline auto Motore::lCartellaIn(float x, float y) const -> int {
    if(lModalita!=0 || x<lDividerX()+10*dpi || x>LW-10*dpi) return -1;
    for(int i=0;i<(int)lCartelle.size();++i) {
        const float top=lCartellaY(i);
        if(y>=top && y<top+36*dpi) return i;
    }
    return -1;
}
inline auto Motore::lCartellaNomeIn(float x, float y) const -> int {
    if(lModalita!=0) return -1;
    for(int i=0;i<(int)lCartelle.size();++i) {
        const float top=lCartellaY(i);
        const float left=lDividerX()+28*dpi, right=LW-140*dpi;
        if(x>=left && x<right && y>=top+1*dpi && y<top+35*dpi) return i;
    }
    return -1;
}
inline auto Motore::lCartellaPiuIn(float x, float y) const -> int {
    const int k=lCartellaIn(x,y);
    if(k<0) return -1;
    const float rr=LW-14*dpi, top=lCartellaY(k);
    return x>=rr-78*dpi && x<rr-48*dpi && y>=top && y<top+36*dpi ? k : -1;
}
inline auto Motore::lCartellaFrecciaIn(float x,float y) const -> int {
    const int k=lCartellaIn(x,y);
    if(k<0) return -1;
    const float rr=LW-14*dpi, top=lCartellaY(k);
    return x>=rr-36*dpi && x<rr && y>=top && y<top+36*dpi ? k : -1;
}
inline auto Motore::lAppCartellaIn(float x, float y) const -> int {
    if(lModalita!=0 || lCartellaAperta<0 || lCartellaAperta>=(int)lCartelle.size()) return -1;
    const int k=lCartellaAperta;
    const float top=lCartellaY(k), left=lDividerX()+24*dpi, right=LW-24*dpi;
    const float bodyTop=top+39*dpi;
    if(x<left || x>=right || y<bodyTop || y>=bodyTop+75*dpi) return -1;
    const int col=x<(left+right)*0.5f?0:1;
    const int row=(int)((y-bodyTop)/(25*dpi)), slot=row*2+col;
    const auto app=lAppCategoria(k);
    return slot>=0 && slot<(int)app.size() && slot<6 ? app[slot] : -1;
}
inline auto Motore::lAppCartellaRimuoviIn(float x, float y) const -> int {
    const int appIndex=lAppCartellaIn(x,y);
    if(appIndex<0 || lCartellaAperta<0 || lCartellaAperta>=(int)lCartelle.size()) return -1;
    const auto apps=lAppCategoria(lCartellaAperta);
    const auto it=std::find(apps.begin(),apps.end(),appIndex);
    if(it==apps.end()) return -1;
    const int slot=(int)(it-apps.begin()), col=slot%2, row=slot/2;
    const float rx=lDividerX()+14*dpi, rr=LW-14*dpi;
    const float cw=(rr-rx-22*dpi)*0.5f;
    const float xx=rx+10*dpi+col*cw, yy=lCartellaY(lCartellaAperta)+39*dpi+row*25*dpi;
    return x>=xx+cw-22*dpi && x<xx+cw-4*dpi && y>=yy+3*dpi && y<yy+21*dpi ? appIndex : -1;
}
inline auto Motore::lGestisciAppIn(float x, float y) const -> int {
    if(lModalita!=2 || lCartellaGestita<0 || lCartellaGestita>=(int)lCartelle.size()) return -1;
    const float left=lDividerX()+28*dpi, right=LW-28*dpi, top=180*dpi;
    const float cardBottom=lFooterTop()-78*dpi;
    if(x<left || x>=right || y<top || y>=cardBottom) return -1;
    const float cw=(right-left-10*dpi)*0.5f, rowH=35*dpi;
    const int col=std::clamp((int)((x-left)/(cw+10*dpi)),0,1);
    const int row=(int)std::floor((y-top+lScrollEditor)/rowH);
    const int k=row*2+col;
    return k>=0 && k<(int)tutteApp.size() ? k : -1;
}
inline auto Motore::lNuovaAppIn(float x, float y) const -> int {
    if(lModalita!=3) return -1;
    const float left=lDividerX()+28*dpi, right=LW-28*dpi, top=234*dpi;
    const float bottom=lFooterTop()-82*dpi;
    if(x<left || x>=right || y<top || y>=bottom) return -1;
    const float cw=(right-left-10*dpi)*0.5f, rowH=35*dpi;
    const int col=std::clamp((int)((x-left)/(cw+10*dpi)),0,1);
    const int row=(int)std::floor((y-top+lScrollEditor)/rowH);
    const int k=row*2+col;
    return k>=0 && k<(int)tutteApp.size() ? k : -1;
}
inline auto Motore::lAppElencoIn(float x, float y) const -> int { return lModalita==1 ? lCellaIn(x,y) : -1; }

// Misure e coordinate dell'elenco alfabetico, mostrato nella modalita' All Apps.
inline auto Motore::lColonne() const -> float { return lVistaLista ? 1.0f : 2.0f; }
inline auto Motore::lCellaW() const -> float {
    const float sx=lModalita==1?lDividerX()+14*dpi:16*dpi;
    return lVistaLista ? LW-sx-16*dpi : (LW-sx-32*dpi)/lColonne();
}
inline auto Motore::lCellaH() const -> float { return lVistaLista ? 52*dpi : 104*dpi; }
inline auto Motore::lAllTop() const -> float { return 78*dpi; }

inline auto Motore::lListaY(int i) const -> float {
    float y=0; wchar_t precedente=0;
    for(int k=0;k<=i && k<(int)filtrate.size();++k) {
        const std::wstring& nome=tutteApp[filtrate[k]].nome;
        const wchar_t lettera=nome.empty()?L'#':(wchar_t)towupper(nome[0]);
        if(k==0 || lettera!=precedente) { y+=22*dpi; precedente=lettera; }
        if(k==i) return y;
        y+=lCellaH();
    }
    return y;
}
inline auto Motore::lListaAlto() const -> float {
    float y=0; wchar_t precedente=0;
    for(int k=0;k<(int)filtrate.size();++k) {
        const std::wstring& nome=tutteApp[filtrate[k]].nome;
        const wchar_t lettera=nome.empty()?L'#':(wchar_t)towupper(nome[0]);
        if(k==0 || lettera!=precedente) { y+=22*dpi; precedente=lettera; }
        y+=lCellaH();
    }
    return y;
}
inline auto Motore::caricaFotoProfilo() -> bool {
    if(lFotoProfiloTentata) return lFotoProfilo!=nullptr;
    lFotoProfiloTentata=true;
    wchar_t appdata[MAX_PATH]=L"";
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,appdata))) return false;
    const std::wstring cartella=std::wstring(appdata)+L"\\Microsoft\\Windows\\AccountPictures";
    WIN32_FIND_DATAW fd{};
    HANDLE h=FindFirstFileW((cartella+L"\\*").c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE) return false;
    std::vector<std::wstring> candidati;
    do {
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring nome=fd.cFileName;
        const size_t dot=nome.find_last_of(L'.');
        if(dot==std::wstring::npos) continue;
        std::wstring ext=nome.substr(dot);
        for(auto& ch:ext) ch=(wchar_t)towlower(ch);
        if(ext==L".png"||ext==L".jpg"||ext==L".jpeg"||ext==L".bmp"||ext==L".jfif")
            candidati.push_back(cartella+L"\\"+nome);
    } while(FindNextFileW(h,&fd));
    FindClose(h);
    std::sort(candidati.begin(),candidati.end(),[](const std::wstring& a,const std::wstring& b) {
        WIN32_FILE_ATTRIBUTE_DATA da{},db{};
        GetFileAttributesExW(a.c_str(),GetFileExInfoStandard,&da);
        GetFileAttributesExW(b.c_str(),GetFileExInfoStandard,&db);
        return CompareFileTime(&da.ftLastWriteTime,&db.ftLastWriteTime)>0;
    });
    for(const auto& percorso:candidati) {
        IShellItemImageFactory* fab=nullptr;
        if(FAILED(SHCreateItemFromParsingName(percorso.c_str(),nullptr,IID_PPV_ARGS(&fab)))||!fab) continue;
        HBITMAP hb=nullptr;
        const HRESULT hr=fab->GetImage({64,64},SIIGBF_THUMBNAILONLY|SIIGBF_BIGGERSIZEOK,&hb);
        fab->Release();
        if(FAILED(hr)||!hb) continue;
        BITMAP bm{}; GetObject(hb,sizeof(bm),&bm);
        const int w=bm.bmWidth,hh=bm.bmHeight;
        if(w<1||hh<1||w>512||hh>512) { DeleteObject(hb); continue; }
        std::vector<UINT> px((size_t)w*hh);
        BITMAPINFO bi{}; bi.bmiHeader={sizeof(BITMAPINFOHEADER),w,-hh,1,32,BI_RGB};
        HDC scr=GetDC(nullptr);
        const int letti=GetDIBits(scr,hb,0,hh,px.data(),&bi,DIB_RGB_COLORS);
        ReleaseDC(nullptr,scr); DeleteObject(hb);
        if(!letti) continue;
        const float cx=(w-1)*0.5f,cy=(hh-1)*0.5f,rad=std::min(w,hh)*0.5f;
        for(int yy=0;yy<hh;++yy) for(int xx=0;xx<w;++xx) {
            UINT& p=px[(size_t)yy*w+xx];
            const float dx=xx-cx,dy=yy-cy;
            UINT a=p>>24;
            if(dx*dx+dy*dy>rad*rad) a=0;
            else if(a==0) a=255;
            UINT rr=(p>>16)&255,gg=(p>>8)&255,bb=p&255;
            rr=rr*a/255; gg=gg*a/255; bb=bb*a/255;
            p=(a<<24)|(rr<<16)|(gg<<8)|bb;
        }
        lFotoProfilo=bitmapDaPixel(w,hh,px);
        if(lFotoProfilo) return true;
    }
    return false;
}

inline auto Motore::lPinnedCella(int i) const -> D2D1_RECT_F {
    const float w=(lDividerX()-40*dpi)/2.0f;
    const float x=16*dpi+(i%2)*(w+8*dpi);
    const float y=122*dpi+(i/2)*54*dpi;
    return {x,y,x+w,y+48*dpi};
}
inline auto Motore::lPinnedIn(float x,float y) const -> int {
    if(lModalita==4 || !query.empty()) return -1;
    for(int i=0;i<(int)lPinned.size() && i<6;++i) {
        const D2D1_RECT_F r=lPinnedCella(i);
        if(x>=r.left && x<r.right && y>=r.top && y<r.bottom) return i;
    }
    return -1;
}
inline auto Motore::lRettBarra() const -> D2D1_RECT_F { return {LW-18*dpi,lGrigliaTop(),LW,lGrigliaBottom()}; }
inline auto Motore::lCursoreBarra() const -> D2D1_RECT_F {
    const double mx=lScrollMax();
    const float alt=lGrigliaBottom()-lGrigliaTop(),tot=alt+(float)mx;
    const float h=std::max(30*dpi,alt*alt/tot),y=lGrigliaTop()+(mx>0?(alt-h)*(float)(lScroll/mx):0);
    return {LW-18*dpi,y,LW-18*dpi+1*dpi,y+h};
}
inline auto Motore::lGrigliaTop() const -> float {
    if(lModalita==1) return 146*dpi;
    if(lModalita==4) return 112*dpi;
    return lAllTop()+42*dpi;
}
inline auto Motore::lGrigliaBottom() const -> float { return lFooterTop()-12*dpi-(float)lFileH; }
inline auto Motore::lScrollMax() const -> double {
    const double contenuto=lVistaLista?(double)lListaAlto()
        :(double)(((int)filtrate.size()+1)/2*lCellaH());
    return std::max(0.0,contenuto-(lGrigliaBottom()-lGrigliaTop()));
}
inline auto Motore::lCella(int i) const -> D2D1_RECT_F {
    if(lModalita==4) {
        const float y=lGrigliaTop()+i*68*dpi-(float)lScroll;
        return {16*dpi,y,std::min(LW*0.5f,LW-16*dpi),y+58*dpi};
    }
    if(lVistaLista) {
        const float sx=lModalita==1?lDividerX()+14*dpi:16*dpi;
        const float y=lGrigliaTop()+lListaY(i)-(float)lScroll;
        return {sx,y,LW-16*dpi,y+lCellaH()};
    }
    const float sx=lModalita==1?lDividerX()+14*dpi:24*dpi;
    const float sw=LW-sx-16*dpi;
    const float cw=(sw-8*dpi)/2.0f;
    const float x=sx+(i%2)*(cw+8*dpi);
    const float y=lGrigliaTop()+(i/2)*lCellaH()-(float)lScroll;
    return {x,y,x+cw,y+lCellaH()};
}
inline auto Motore::lCellaIn(float x,float y) const -> int {
    if((lModalita!=1 && lModalita!=4) || y<lGrigliaTop() || y>lGrigliaBottom()) return -1;
    for(int i=0;i<(int)filtrate.size();++i) {
        const auto r=lCella(i);
        if(x>=r.left && x<r.right && y>=r.top && y<r.bottom) return i;
    }
    return -1;
}
// prende il primo piano anche quando lo apre il tasto Win (nessun clic arrivato al nostro processo)
inline auto Motore::prendiFuoco(HWND w) -> void {
    SetForegroundWindow(w);
    if (GetForegroundWindow() == w) return;
    HWND fg = GetForegroundWindow();
    const DWORD a = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, b = GetCurrentThreadId();
    // (mai agganciati a un'app che non risponde: il suo input bloccato bloccherebbe anche quello di FluidWin)
    if (a && a != b && !IsHungAppWindow(fg) && AttachThreadInput(b, a, TRUE)) { SetForegroundWindow(w); SetFocus(w); AttachThreadInput(b, a, FALSE); }
}

// Apre il launcher (o lo chiude, se e' gia' aperto): cresce dall'icona "App" del dock con un piccolo rimbalzo
inline auto Motore::apriLauncher() -> void {
    if (!attivo) return;
    // anche sopra un gioco o un video a tutto schermo (tasto Win): il dock torna a disegnarsi mentre il launcher e' aperto
    if (nascostoSchermoIntero) schermoIntero(false);
    if (lAperto && !lChiusura) { chiudiLauncher(); return; }
    // si sta chiudendo: torna indietro da dove e' arrivato (stessa molla, stessa velocita'), senza ripartire da capo.
    // Prima c'era anche un blocco di 250 ms dopo la chiusura in cui i clic si ignoravano: non serve piu', il clic
    // sull'icona App non chiude piu' il launcher perdendo il fuoco (vedi WM_ACTIVATE)
    if (lAperto && lChiusura) {
        lChiusura = false; lDirty = true; lRipreso = true;
        // una ricerca dei file arrivata durante la chiusura e' stata scartata: si rifa' (R20261119-195000-S3)
        if (!query.empty() && lFileTesto != query) SetTimer(launcher, TIMER_FILE_L, 200, nullptr);
        prendiFuoco(launcher);
        scrivi("launcher: riaperto mentre si chiudeva");
        return;
    }
    if (tutteApp.empty()) leggiTutteApp(true);
    else if (adesso() - lLetto > 600) leggiTutteApp();
    // centro di controllo aperto: prima si chiude lui, poi (TIMER_DOPO) si apre il launcher; mai uno sopra l'altro
    if (!nessunaScheda()) { if (dopo != 1) { chiudiTutte(); apriDopo(1, 0); } return; }
    primaDelLauncher = GetForegroundWindow();
    query.clear(); filtra(); lModalita = 0; lCartellaAperta = 0;
    lSopra = -1; lSopraPinned = -1; lSopraVista = lSopraProfilo = false;
    lScroll = lScrollObj = 0; lFileH = 0;   // (la sezione dei file riparte chiusa)
    lAperto = true; lChiusura = false; lScala = 0.12; lVel = 0; lAlfa = 0; lT0 = adesso(); lRegione = false; lRipreso = false;
    // sopra il dock, al centro; la finestra scende fino all'icona "App". Col dock in alto: sotto, al centro; ai lati:
    // accanto al dock, a meta' altezza. Li' la finestra non arriva all'icona: cresce dal suo bordo piu' vicino all'icona
    float ax = W / 2.0f;
    const auto pos = posizioniRiposo();
    for (size_t i = 0; i < voci.size(); i++) if (voci[i].avvio == AVVIO_LAUNCHER) ax = pos[i] + S / 2;
    const RECT ic = aSchermo({ax - S / 2, yBaseRiposo() - S, ax + S / 2, yBaseRiposo()});   // l'icona App a riposo
    const POINT o = origine();
    const D2D1_POINT_2F bi = fis(0, yBaseRiposo() - S - PAD);   // bordo interno del dock (nella finestra del dock)
    const int stacco = (int)std::round(18 * dpi);
    int x0 = (int)schermo.left + (int)std::round((schermo.right - schermo.left - LW) * 0.5f), y0 = 0;
    if (lato == 0) {
        x0 = std::clamp(x0, (int)schermo.left + 8, std::max((int)schermo.left + 8, (int)schermo.right - 8 - (int)LW));
        const int y1 = o.y + (int)std::round(bi.y) - stacco;
        // schermi bassi (1366x768 al 125%: ~520 punti sopra il dock): il launcher si accorcia invece di uscire in alto
        LH = std::clamp((float)(y1 - schermo.top) - 12 * dpi, 300 * dpi, LHMAX);
        y0 = y1 - (int)LH;
    } else if (lato == 1) {
        x0 = std::clamp((int)schermo.left + (int)std::round((schermo.right - schermo.left - LW) * 0.5f), (int)schermo.left + 8, std::max((int)schermo.left + 8, (int)schermo.right - 8 - (int)LW));
        y0 = o.y + (int)std::round(bi.y) + stacco;
        LH = std::clamp((float)(schermo.bottom - y0) - 12 * dpi, 300 * dpi, LHMAX);
    } else {
        LH = std::clamp((float)(schermo.bottom - schermo.top) - 24 * dpi, 300 * dpi, LHMAX);
        y0 = schermo.top + (int)std::round((schermo.bottom - schermo.top - LH) / 2);
        x0 = lato == 2 ? o.x + (int)std::round(bi.x) + stacco : o.x + (int)std::round(bi.x) - stacco - (int)LW;
        // (schermo stretto: dentro lo schermo anche a costo di coprire un poco il dock, R20261120-190507-S2)
        x0 = std::clamp(x0, (int)schermo.left + 8, std::max((int)schermo.left + 8, (int)schermo.right - 8 - (int)LW));
    }
    rettLauncher = {x0, y0, x0 + (int)LW, y0 + (int)LH};
    const float icx = (ic.left + ic.right) / 2.0f - x0, icy = (ic.top + ic.bottom) / 2.0f - y0;
    switch (lato) {
        case 1: ancora = {(LONG)std::clamp(icx, 40 * dpi, LW - 40 * dpi), 0}; break;
        case 2: ancora = {0, (LONG)std::clamp(icy, 40 * dpi, LH - 40 * dpi)}; break;
        case 3: ancora = {(LONG)LW, (LONG)std::clamp(icy, 40 * dpi, LH - 40 * dpi)}; break;
        default: ancora = {(LONG)std::clamp(icx, 40 * dpi, LW - 40 * dpi), (LONG)std::min(LH + LEXTRA - 4, icy)};
    }
    SetWindowRgn(launcher, nullptr, FALSE);
    SetWindowPos(launcher, HWND_TOPMOST, x0, rettLauncher.top, (int)LW, (int)(LH + LEXTRA), SWP_NOACTIVATE);
    posizionaLauncher();
    ShowWindow(vetroLauncher, SW_SHOWNOACTIVATE);
    ShowWindow(launcher, SW_SHOW);
    prendiFuoco(launcher);
    lDirty = true; dirty = true;
    scrivi(GetForegroundWindow() == launcher ? "launcher aperto" : "launcher aperto senza fuoco");
}

inline auto Motore::chiudiLauncher() -> void {
    if (!lAperto || lChiusura) return;
    lChiusura = true; lDirty = true;
    if (lRegione) { SetWindowRgn(launcher, nullptr, FALSE); lRegione = false; }   // per rientrare nell'icona serve tutta la finestra
}

inline auto Motore::nascondiLauncher() -> void {
    // chiuso senza aprire un'app (Esc, Win, clic sul dock): il primo piano torna alla finestra di prima
    if (lAperto && GetForegroundWindow() == launcher && primaDelLauncher && IsWindow(primaDelLauncher) && IsWindowVisible(primaDelLauncher))
        SetForegroundWindow(primaDelLauncher);
    primaDelLauncher = nullptr;
    if (launcher) ShowWindow(launcher, SW_HIDE);
    if (vetroLauncher) ShowWindow(vetroLauncher, SW_HIDE);
    if (lAperto) lChiusoT = adesso();
    lAperto = false; lChiusura = false; lAnim = false; lDirty = false;
    dirty = true;   // il dock puo' tornare a nascondersi
}

// il contenuto (DirectComposition) e il vetro crescono attorno all'icona "App"
inline auto Motore::posizionaLauncher() -> void {
    const double s = lScala;
    const float ax = (float)ancora.x, ay = (float)ancora.y;
    visualL->SetTransform(D2D1::Matrix3x2F::Scale((float)s, (float)s, {ax, ay}));
    effettoL->SetOpacity((float)std::clamp(lAlfa, 0.0, 1.0));
    const RECT& r = rettLauncher;
    const double gx = r.left + ax, gy = r.top + ay;
    const int L = (int)std::lround(gx + (r.left - gx) * s), T = (int)std::lround(gy + (r.top - gy) * s);
    const int R = (int)std::lround(gx + (r.right - gx) * s), B = (int)std::lround(gy + (r.bottom - gy) * s);
    SetWindowPos(vetroLauncher, launcher, L, T, std::max(1, R - L), std::max(1, B - T), SWP_NOACTIVATE);
    SetLayeredWindowAttributes(vetroLauncher, 0, (BYTE)std::lround(std::clamp(lAlfa, 0.0, 1.0) * 255), LWA_ALPHA);
    dcomp->Commit();
}

inline auto Motore::avviaApp(int i) -> void {
    if (i < 0 || i >= (int)filtrate.size()) return;
    const AppL& a = tutteApp[filtrate[i]];
    primaDelLauncher = nullptr;   // il primo piano va all'app che si apre
    chiudiLauncher();
    ShellExecuteW(nullptr, L"open", a.avvio.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

inline auto Motore::avviaAppDiretto(int indiceApp) -> void {
    if (indiceApp < 0 || indiceApp >= (int)tutteApp.size()) return;
    const std::wstring avvio = tutteApp[indiceApp].avvio;
    primaDelLauncher = nullptr;
    chiudiLauncher();
    ShellExecuteW(nullptr, L"open", avvio.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

inline auto Motore::rettProfilo() const -> D2D1_RECT_F { return {16*dpi, lFooterTop()+6*dpi, 270*dpi, lFooterTop()+46*dpi}; }
inline auto Motore::rettEnergia() const -> D2D1_RECT_F { return {LW-72*dpi, lFooterTop()+6*dpi, LW-16*dpi, lFooterTop()+46*dpi}; }

// Menu di Windows per l'app (lo stesso dello Start), con in cima "Aggiungi al dock"
inline auto Motore::menuApp(int indiceApp, POINT pt) -> void {
    // menuApp riceve sempre l'indice reale in tutteApp, anche dal Workspace fissato.
    if (indiceApp < 0 || indiceApp >= (int)tutteApp.size()) return;
    const AppL a = tutteApp[indiceApp];
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(a.avvio.c_str(), nullptr, IID_PPV_ARGS(&it)))) return;
    IContextMenu* cm = nullptr;
    it->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
    // eseguibile e AppUserModelID dell'app, per il collegamento nel dock
    std::wstring exe, id;
    IShellItem2* it2 = nullptr;
    if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
        LPWSTR s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &s)) && s) { exe = s; CoTaskMemFree(s); }
        s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_AppUserModel_ID, &s)) && s) { id = s; CoTaskMemFree(s); }
        it2->Release();
    }
    it->Release();
    if (!cm) return;
    const UINT ID_DOCK = 0x7F00, ID_PIN_START = 0x7F01;
    const bool giaFissata=std::find(lPinnedManual.begin(),lPinnedManual.end(),a.avvio)!=lPinnedManual.end();
    HMENU m = CreatePopupMenu();
    if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, ID_DOCK - 1, CMF_NORMAL))) {
        cm->QueryInterface(IID_PPV_ARGS(&menuShell2)); cm->QueryInterface(IID_PPV_ARGS(&menuShell3));
        // Le azioni FluidWin vengono prima delle voci shell.
        auto pinVoce=menu::voce(giaFissata?testi::t(L"Rimuovi da Start Menu",L"Unpin from Start Menu")
                                          :testi::t(L"Fissa a Start",L"Pin to Start"),ID_PIN_START);
        pinVoce.simbolo=menu::I_BARRA;
        std::vector<menu::Voce> voci = {
            menu::titolo(a.nome),
            pinVoce,
            menu::voce(testi::t(L"Aggiungi al dock",L"Add to dock"),ID_DOCK)
        };
        // "Disinstalla" (la voce di Windows, per le app dello Store e per le altre) in cima, subito visibile
        UINT idDisinstalla = 0;
        for (int i = 0; i < GetMenuItemCount(m) && !idDisinstalla; i++) {
            const UINT id = GetMenuItemID(m, i);
            if (id == (UINT)-1 || id == 0) continue;
            wchar_t v[64] = L"";
            if (SUCCEEDED(cm->GetCommandString(id - 1, GCS_VERBW, nullptr, (LPSTR)v, 64)) && !_wcsicmp(v, L"Uninstall")) idDisinstalla = id;
        }
        if (idDisinstalla) voci.push_back(menu::voce(testi::t(L"Disinstalla…", L"Uninstall…"), idDisinstalla));
        voci.push_back(menu::separatore());
        // Windows contributes a separate "Unpin from Start menu" command which only edits
        // the native Start list. Hide it here: FluidWin's own entry above updates Workspace.ini.
        for (auto& v : menu::daHmenu(m)) {
            std::wstring testo=v.testo, verbo;
            std::transform(testo.begin(),testo.end(),testo.begin(),[](wchar_t ch){return (wchar_t)towlower(ch);});
            bool unpinShell=testo.find(L"unpin from start")!=std::wstring::npos ||
                             testo.find(L"rimuovi da start")!=std::wstring::npos;
            if(v.id && v.id!=(UINT)-1) {
                wchar_t nomeVerbo[128]=L"";
                if(SUCCEEDED(cm->GetCommandString(v.id-1,GCS_VERBW,nullptr,(LPSTR)nomeVerbo,128))) {
                    verbo=nomeVerbo;
                    std::transform(verbo.begin(),verbo.end(),verbo.begin(),[](wchar_t ch){return (wchar_t)towlower(ch);});
                    if(verbo.find(L"unpinfromstart")!=std::wstring::npos) unpinShell=true;
                }
            }
            if(unpinShell) continue;
            if(v.id!=idDisinstalla || v.separatore) voci.push_back(v);
        }
        // i sottomenu ("Invia a", "Apri con"...) la shell li riempie solo quando stanno per aprirsi
        auto prepara = [this](HMENU s) {
            if (menuShell3) { LRESULT r = 0; menuShell3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)s, 0, &r); }
            else if (menuShell2) menuShell2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)s, 0);
        };
        const int temaMenu=coloreAuto?(temaScuro()?1:0):(fondoScuro?1:0);
        const UINT cmd = menu::mostra(voci,pt,launcher,false,prepara,temaMenu);
        if (menuShell3) { menuShell3->Release(); menuShell3 = nullptr; }
        if (menuShell2) { menuShell2->Release(); menuShell2 = nullptr; }
        if (cmd == ID_PIN_START) {
            togglePinned(indiceApp);
        } else if (cmd == ID_DOCK) {
            aggiungiAlDock(a.nome, exe, id, a.avvio);
        } else if (cmd > 0) {
            CMINVOKECOMMANDINFOEX ci{}; ci.cbSize = sizeof(ci); ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            ci.hwnd = launcher; ci.lpVerb = MAKEINTRESOURCEA(cmd - 1); ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
            ci.nShow = SW_SHOWNORMAL; ci.ptInvoke = pt;
            cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci);
            primaDelLauncher = nullptr;   // ha aperto qualcosa: il primo piano va a quello
            chiudiLauncher();
            appDaAggiornare = true;       // "Aggiungi alla barra" e simili cambiano le icone fisse del dock
        }
    }
    DestroyMenu(m);
    cm->Release();
}

// Collegamento nella cartella delle app fisse della barra (quella che legge il dock)
inline auto Motore::aggiungiAlDock(const std::wstring& nome, const std::wstring& exe, const std::wstring& id, const std::wstring& avvio) -> void {
    wchar_t cart[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, cart))) return;
    std::wstring n = nome;
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const std::wstring file = std::wstring(cart) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\" + n + L".lnk";
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return;   // gia' nel dock
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    bool ok = false;
    if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) ok = SUCCEEDED(sl->SetPath(exe.c_str()));
    else {   // app dello Store: il collegamento punta alla voce della cartella App
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(avvio.c_str(), nullptr, &pidl, 0, nullptr))) { ok = SUCCEEDED(sl->SetIDList(pidl)); CoTaskMemFree(pidl); }
    }
    if (ok && !id.empty()) {   // cosi' il dock riconosce le finestre dell'app
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
            PROPVARIANT pv; if (SUCCEEDED(InitPropVariantFromString(id.c_str(), &pv))) { ps->SetValue(PKEY_AppUserModel_ID, pv); ps->Commit(); PropVariantClear(&pv); }
            ps->Release();
        }
    }
    IPersistFile* pf = nullptr;
    if (ok && SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(file.c_str(), TRUE); pf->Release(); }
    sl->Release();
    appDaAggiornare = true;
    if (registro) { char b[200]; snprintf(b, 200, "aggiunta al dock: %ls", n.c_str()); scrivi(b); }
}

// Accensione: come il pulsante dello Start
inline auto Motore::menuEnergia(POINT pt) -> void {
    const int s = (int)menu::mostra({menu::voce(testi::t(L"Sospendi", L"Sleep"), 1), menu::voce(testi::t(L"Riavvia", L"Restart"), 2),
                                     menu::voce(testi::t(L"Arresta il sistema", L"Shut down"), 3), menu::separatore(),
                                     menu::voce(testi::t(L"Blocca", L"Lock"), 4), menu::voce(testi::t(L"Disconnetti", L"Sign out"), 5)}, pt, launcher);
    lSopraEnergia = false; lDirty = true;
    if (s <= 0) return;
    chiudiLauncher();
    energia(s);
}

inline auto Motore::energia(int s) -> void {
    if (s == 4) { LockWorkStation(); return; }
    // privilegio di arresto: ce l'ha ogni utente, va solo attivato per questo processo
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    const DWORD motivo = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    // le app con lavoro non salvato possono fermare riavvio e arresto (nessun EWX_FORCE), come dallo Start
    if (s == 1) SetSuspendState(FALSE, FALSE, FALSE);
    else if (s == 2) ExitWindowsEx(EWX_REBOOT, motivo);
    else if (s == 3) ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | 0x00400000 /* EWX_HYBRID_SHUTDOWN: avvio rapido */, motivo);
    else if (s == 5) ExitWindowsEx(EWX_LOGOFF, 0);
}

inline auto Motore::selezionaVisibile() -> void {
    const auto r = lCella(lSel);
    const float top = lGrigliaTop(), bot = lGrigliaBottom();
    if (r.top < top) lScrollObj -= top - r.top;
    else if (r.bottom > bot) lScrollObj += r.bottom - bot;
    lScrollObj = std::clamp(lScrollObj, 0.0, lScrollMax());
}

// icona "App" nel dock: il logo di Windows 11 (quattro riquadri azzurri sfumati) su vetro chiaro
// Icona Start: logo Windows senza il riquadro bianco, oppure una variante Apple.
inline auto Motore::disegnaIconaApp(const D2D1_RECT_F& r, float alfa, bool scuro) -> void {
    const float d = r.right - r.left;
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;

    if (logoStart == 2 && iconaStartPersonale) {
        // Bitmap personale gia' decodificata e scalata al caricamento: si mantiene il rapporto d'aspetto.
        const D2D1_SIZE_F bs = iconaStartPersonale->GetSize();
        const float latoMax = d * 0.89f;
        const float k = std::min(latoMax / std::max(1.0f, bs.width), latoMax / std::max(1.0f, bs.height));
        const float w = bs.width * k, h = bs.height * k;
        const D2D1_RECT_F icona = {cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f};
        dc->DrawBitmap(iconaStartPersonale, icona, alfa, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }

    if (logoStart == 1 && iconaStartMacOS) {
        // The Finder PNG has transparent margins; crop those before sizing so its visible artwork
        // reaches approximately the same visual bounds as the other dock application icons.
        const D2D1_SIZE_F bs = iconaStartMacOS->GetSize();
        const D2D1_RECT_F sorgente = {bs.width * 0.085f, bs.height * 0.105f,
                                      bs.width * 0.915f, bs.height * 0.895f};
        const float sw = sorgente.right - sorgente.left, sh = sorgente.bottom - sorgente.top;
        const float scala = std::min(d * 0.92f / std::max(1.0f, sw), d * 0.92f / std::max(1.0f, sh));
        const float w = sw * scala, h = sh * scala;
        const D2D1_RECT_F destinazione = {cx - w * 0.5f, cy - h * 0.5f,
                                          cx + w * 0.5f, cy + h * 0.5f};
        dc->DrawBitmap(iconaStartMacOS, destinazione, alfa,
                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &sorgente);
        return;
    }

    if (logoStart == 1) {
        // Fallback if the bundled Finder image is unavailable: previous built-in macOS dock mark.
        const D2D1_RECT_F base = {cx - d * 0.44f, cy - d * 0.31f, cx + d * 0.44f, cy + d * 0.31f};
        pennello->SetColor(D2D1::ColorF(0.42f, 0.48f, 0.58f, 0.66f * alfa));
        dc->FillRoundedRectangle({base, d * 0.15f, d * 0.15f}, pennello);
        pennello->SetColor(D2D1::ColorF(1, 1, 1, 0.55f * alfa));
        dc->DrawRoundedRectangle({{base.left + 0.5f, base.top + 0.5f, base.right - 0.5f, base.bottom - 0.5f}, d * 0.15f, d * 0.15f}, pennello, 1.0f);
        const D2D1_COLOR_F colori[4] = {
            D2D1::ColorF(0.23f, 0.64f, 0.98f, alfa), D2D1::ColorF(0.95f, 0.42f, 0.34f, alfa),
            D2D1::ColorF(0.29f, 0.78f, 0.50f, alfa), D2D1::ColorF(0.72f, 0.49f, 0.94f, alfa)
        };
        const float latoApp = d * 0.23f, passo = d * 0.25f;
        for (int i = 0; i < 4; i++) {
            const float x = cx - d * 0.34f + i * passo;
            const D2D1_RECT_F app = {x, cy - latoApp * 0.45f, x + latoApp, cy + latoApp * 0.45f};
            pennello->SetColor(colori[i]);
            dc->FillRoundedRectangle({app, d * 0.035f, d * 0.035f}, pennello);
        }
        // separatore e cestino in miniatura, come la coda del Dock macOS
        pennello->SetColor(D2D1::ColorF(1, 1, 1, 0.42f * alfa));
        dc->DrawLine({cx + d * 0.24f, cy - d * 0.20f}, {cx + d * 0.24f, cy + d * 0.20f}, pennello, 0.8f * d);
        dc->DrawRoundedRectangle({{cx + d * 0.29f, cy - d * 0.10f, cx + d * 0.39f, cy + d * 0.17f}, d * 0.025f, d * 0.025f}, pennello, 1.0f * d);
        return;
    }

    // Simbolo Windows 11 alla stessa scala visiva delle icone applicazione, senza piastrella.
    const float lato = d * 0.86f, fessura = d * 0.045f, q = (lato - fessura) * 0.5f;
    const float x0 = cx - lato * 0.5f, y0 = cy - lato * 0.5f;
    const D2D1_GRADIENT_STOP fermate[2] = {
        {0, D2D1::ColorF(0.33f, 0.80f, 1.00f, alfa)},
        {1, D2D1::ColorF(0.00f, 0.42f, 0.86f, alfa)}
    };
    ID2D1GradientStopCollection* fc = nullptr;
    ID2D1LinearGradientBrush* sf = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(fermate, 2, &fc)))
        dc->CreateLinearGradientBrush({{x0, y0}, {x0 + lato, y0 + lato}}, fc, &sf);
    if (!sf) pennello->SetColor(D2D1::ColorF(0.10f, 0.58f, 0.95f, alfa));
    for (int k = 0; k < 4; k++) {
        const float x = x0 + (k % 2) * (q + fessura), y = y0 + (k / 2) * (q + fessura);
        const D2D1_RECT_F p{x, y, x + q, y + q};
        const float ra = d * 0.012f;
        if (sf) dc->FillRoundedRectangle({p, ra, ra}, sf);
        else dc->FillRoundedRectangle({p, ra, ra}, pennello);
    }
    if (sf) sf->Release();
    if (fc) fc->Release();
}

// restituisce false se il launcher si e' appena chiuso
inline auto Motore::renderLauncher() -> bool {
    bool anim = molla(lScala, lVel, lChiusura ? 0.0 : 1.0, 240, lChiusura ? 0.8 : 0.55, 0.0005);
    for(int i=0;i<6;++i) {
        const double obiettivo=(i<(int)lPinned.size() && i==lSopraPinned)?1.045:1.0;
        const double prossima=verso(lScalaPinnedHover[i],obiettivo,0.075);
        if(prossima!=lScalaPinnedHover[i]) { lScalaPinnedHover[i]=prossima; anim=true; }
    }
    {
        const double obiettivo=(lModalita==1 && lSopra>=0)?1.035:1.0;
        const double prossima=verso(lScalaAllHover,obiettivo,0.075);
        if(prossima!=lScalaAllHover) { lScalaAllHover=prossima; anim=true; }
    }
    for(int i=0;i<4;++i) {
        const double obiettivo=(i==lSopraAzione)?1.035:1.0;
        const double prossima=verso(lScalaAzioneHover[i],obiettivo,0.075);
        if(prossima!=lScalaAzioneHover[i]) { lScalaAzioneHover[i]=prossima; anim=true; }
    }
    if (lChiusura) {
        lAlfa = std::clamp((lScala - 0.10) / 0.30, 0.0, 1.0);
        if (lAlfa <= 0.02 || !anim) { nascondiLauncher(); return false; }
    } else {
        lAlfa = verso(lAlfa, 1.0, 0.045);
        if (lAlfa != 1.0) anim = true;
    }
    posizionaLauncher();
    if (!lChiusura && !anim && !lRegione) {
        SetWindowRgn(launcher, CreateRectRgn(0, 0, (int)LW, (int)LH), FALSE); lRegione = true;
    }
    {
        const double fh = verso(lFileH, lFileAlto(), 0.07);
        if (fh != lFileH) {
            lFileH = fh; anim = true;
            if (lSelTastiera && !query.empty() && lSelFile < 0 && lSel >= 0 && lSel < (int)filtrate.size())
                selezionaVisibile();
            lScrollObj = std::clamp(lScrollObj, 0.0, lScrollMax());
        }
    }
    const double sc = verso(lScroll, lScrollObj, 0.06);
    if (sc != lScroll) { lScroll = sc; anim = true; }
    raccogliIcone();
    caricaFotoProfilo();
    // Workspace sections stay fixed; folder chevrons change immediately.

    const bool scuro = coloreAuto ? temaScuro() : fondoScuro;
    const D2D1_COLOR_F ink = inchiostroUI(scuro, 1.0f);
    const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.62f};
    const D2D1_COLOR_F blu = scuro ? D2D1::ColorF(0.32f, 0.64f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.40f, 0.92f, 1.0f);
    dc->SetTarget(bersaglioL);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    disegnaFondo({0.5f, 0.5f, LW - 0.5f, LH - 0.5f}, scuro, opacita, coloreAuto ? nullptr : &coloreFondo);

    // Barra di ricerca universale: la digitazione in qualunque punto della finestra filtra le app.
    {
        const D2D1_RECT_F rc = {16 * dpi, 14 * dpi, LW - 16 * dpi, 54 * dpi};
        pennello->SetColor(scuro ? D2D1::ColorF(0.105f,0.108f,0.12f,0.96f) : D2D1::ColorF(0.94f,0.945f,0.955f,0.90f));
        dc->FillRoundedRectangle({rc, 13 * dpi, 13 * dpi}, pennello);
        pennello->SetColor(tenue);
        const float lx = rc.left + 19 * dpi, ly = (rc.top + rc.bottom) * 0.5f;
        dc->DrawEllipse({{lx,ly},6*dpi,6*dpi},pennello,1.8f*dpi);
        linea(2.5f,2.5f,7,7,lx,ly);
        const D2D1_RECT_F rt = {lx + 18*dpi, rc.top, rc.right - 108*dpi, rc.bottom};
        if (query.empty()) scrivi(testi::t(L"Cerca app, file recenti o esegui un comando...",L"Search apps, recent files, or run a command..."),fmtTitolo,rt);
        else {
            pennello->SetColor(ink);
            scrivi(query,fmtTitolo,rt);
            const float lt = larghezzaTesto(query,fmtTitolo);
            pennello->SetColor(blu);
            dc->DrawLine({rt.left+lt+2*dpi,ly-9*dpi},{rt.left+lt+2*dpi,ly+9*dpi},pennello,1.6f*dpi);
            pennello->SetColor(tenue);
            scrivi(L"×",fmtTitolo,{rc.right-91*dpi,rc.top,rc.right-66*dpi,rc.bottom});
        }
        const D2D1_RECT_F esc={rc.right-54*dpi,rc.top+9*dpi,rc.right-12*dpi,rc.bottom-9*dpi};
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.08f):D2D1::ColorF(0,0,0,0.055f));
        dc->FillRoundedRectangle({esc,6*dpi,6*dpi},pennello);
        pennello->SetColor(tenue);
        scrivi(L"ESC",fmtNome,esc);
    }

    const D2D1_COLOR_F accenti[6]={
        D2D1::ColorF(0.10f,0.72f,0.95f,1),D2D1::ColorF(0.72f,0.40f,0.98f,1),
        D2D1::ColorF(1.00f,0.36f,0.48f,1),D2D1::ColorF(0.10f,0.78f,0.88f,1),
        D2D1::ColorF(0.12f,0.82f,0.62f,1),D2D1::ColorF(0.91f,0.67f,0.22f,1)};
    const wchar_t* iconeCartella[6]={L"⌘",L"♟",L"✿",L"◎",L"✓",L"⚙"};
    const float dx=lDividerX(), leftR=dx-16*dpi, rx=dx+14*dpi, rr=LW-14*dpi;
    auto disegnaFreccia=[&](float cx,float cy,double angolo,float lato) {
        const float co=(float)std::cos(angolo),si=(float)std::sin(angolo);
        auto ruota=[&](float x,float y) { return D2D1::Point2F(cx+x*co-y*si,cy+x*si+y*co); };
        const auto a=ruota(-lato*0.5f,-lato*0.22f), b=ruota(0,lato*0.28f), d=ruota(lato*0.5f,-lato*0.22f);
        dc->DrawLine(a,b,pennello,1.65f*dpi);
        dc->DrawLine(b,d,pennello,1.65f*dpi);
    };

    auto scaleRect=[&](const D2D1_RECT_F& box,float cx,float cy,float factor) -> D2D1_RECT_F {
        return {cx+(box.left-cx)*factor,cy+(box.top-cy)*factor,
                cx+(box.right-cx)*factor,cy+(box.bottom-cy)*factor};
    };

    // Le pagine Folders, All Apps, Manage Folder Apps e New Library Folder condividono la stessa colonna workspace.
    if(lModalita!=4) {
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.08f):D2D1::ColorF(0,0,0,0.07f));
        dc->DrawLine({dx,70*dpi},{dx,lFooterTop()},pennello,1*dpi);

        // Workspace heading shares the typography and muted color of App Library Folders.
        pennello->SetColor(tenue);
        scrivi(testi::t(L"WORKSPACE",L"WORKSPACE"),fmtTitolo,{16*dpi,84*dpi,dx-104*dpi,116*dpi});
        scrivi(std::to_wstring(lPinned.size())+L" pinned",fmtNome,{dx-96*dpi,84*dpi,dx-16*dpi,116*dpi});
        for(int i=0;i<(int)lPinned.size() && i<6;++i) {
            const D2D1_RECT_F base=lPinnedCella(i);
            const float scala=(float)lScalaPinnedHover[i];
            const float cx=(base.left+base.right)*0.5f, cy=(base.top+base.bottom)*0.5f;
            auto scalaBox=[&](const D2D1_RECT_F& box) -> D2D1_RECT_F {
                return {cx+(box.left-cx)*scala,cy+(box.top-cy)*scala,
                        cx+(box.right-cx)*scala,cy+(box.bottom-cy)*scala};
            };
            const D2D1_RECT_F r=scalaBox(base);
            const bool sopra=i==lSopraPinned;
            pennello->SetColor(scuro?D2D1::ColorF(1,1,1,sopra?0.13f:0.055f)
                                     :D2D1::ColorF(1,1,1,sopra?0.82f:0.66f));
            dc->FillRoundedRectangle({r,11*dpi*scala,11*dpi*scala},pennello);
            AppL& app=tutteApp[lPinned[i]];
            const D2D1_RECT_F ri=scalaBox({base.left+7*dpi,base.top+6*dpi,base.left+43*dpi,base.top+42*dpi});
            if(app.icona) dc->DrawBitmap(app.icona,ri,1.0f,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else { pennello->SetColor(tenue); dc->FillRoundedRectangle({ri,7*dpi*scala,7*dpi*scala},pennello); }
            pennello->SetColor(ink);
            scrivi(app.nome,fmtVoce,scalaBox({base.left+52*dpi,base.top+3*dpi,base.right-7*dpi,base.bottom-3*dpi}));
        }

        const float ay=LH-256*dpi;
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.075f):D2D1::ColorF(0,0,0,0.07f));
        dc->DrawLine({16*dpi,ay-15*dpi},{dx-16*dpi,ay-15*dpi},pennello,1*dpi);
        pennello->SetColor(tenue);
        scrivi(testi::t(L"INSTANT ACTIONS",L"INSTANT ACTIONS"),fmtTitolo,{16*dpi,ay,dx-16*dpi,ay+24*dpi});
        const wchar_t* azioni[4]={L"New Terminal Session",L"Capture Screen Region",L"Task Manager",L"Open Downloads"};
        const wchar_t* simboli[4]={L">_",L"⌗",L"▥",L"↓"};
        for(int k=0;k<4;++k) {
            const float top=ay+(28.0f+k*36.0f)*dpi;
            const D2D1_RECT_F base={16*dpi,top,leftR,top+30*dpi};
            const float scala=(float)lScalaAzioneHover[k],cx=(base.left+base.right)*0.5f,cy=(base.top+base.bottom)*0.5f;
            const D2D1_RECT_F ar=scaleRect(base,cx,cy,scala);
            const bool hover=k==lSopraAzione;
            pennello->SetColor(hover?(scuro?D2D1::ColorF(1,1,1,0.12f):D2D1::ColorF(0,0,0,0.085f))
                                    :(scuro?D2D1::ColorF(1,1,1,0.055f):D2D1::ColorF(0,0,0,0.035f)));
            dc->FillRoundedRectangle({ar,9*dpi*scala,9*dpi*scala},pennello);
            pennello->SetColor(blu);
            scrivi(simboli[k],fmtVoce,scaleRect({base.left+12*dpi,base.top,base.left+38*dpi,base.bottom},cx,cy,scala));
            pennello->SetColor(ink);
            scrivi(azioni[k],fmtVoce,scaleRect({base.left+42*dpi,base.top,base.right-12*dpi,base.bottom},cx,cy,scala));
        }
    }

    auto drawAppIcon=[&](int appIndex,const D2D1_RECT_F& box) {
        if(appIndex<0 || appIndex>=(int)tutteApp.size()) return;
        AppL& app=tutteApp[appIndex];
        if(app.icona) dc->DrawBitmap(app.icona,box,1.0f,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else { pennello->SetColor(tenue); dc->FillRoundedRectangle({box,7*dpi,7*dpi},pennello); }
    };
    auto drawAppLibraryRow=[&](int appIndex,const D2D1_RECT_F& base,bool managed,bool pinnedControls,bool hover=false) {
        const AppL& app=tutteApp[appIndex];
        const float cx=(base.left+base.right)*0.5f,cy=(base.top+base.bottom)*0.5f;
        const float scala=hover?(float)lScalaAllHover:1.0f;
        auto rct=[&](const D2D1_RECT_F& box) { return scaleRect(box,cx,cy,scala); };
        const D2D1_RECT_F r=rct(base);
        const D2D1_RECT_F tile=rct({base.left+7*dpi,base.top+6*dpi,base.left+36*dpi,base.top+35*dpi});
        const int cat=lCategoriaApp(app);
        pennello->SetColor(hover?(scuro?D2D1::ColorF(1,1,1,0.095f):D2D1::ColorF(0,0,0,0.075f))
                                :(scuro?D2D1::ColorF(1,1,1,0.035f):D2D1::ColorF(0,0,0,0.025f)));
        dc->FillRoundedRectangle({r,9*dpi*scala,9*dpi*scala},pennello);
        drawAppIcon(appIndex,tile);
        pennello->SetColor(ink);
        scrivi(app.nome,fmtVoce,rct({base.left+44*dpi,base.top+3*dpi,base.right-(pinnedControls?46:10)*dpi,base.top+25*dpi}));
        pennello->SetColor(tenue);
        scrivi(cat>=0?lCartelle[cat].nome:testi::t(L"Non assegnata",L"Unassigned"),fmtSotto,
               rct({base.left+44*dpi,base.top+23*dpi,base.right-(pinnedControls?46:10)*dpi,base.bottom-2*dpi}));
        if(managed) {
            const bool selected=cat==lCartellaGestita;
            const D2D1_RECT_F chk=rct({base.left+7*dpi,base.top+14*dpi,base.left+18*dpi,base.top+25*dpi});
            pennello->SetColor(selected?blu:tenue);
            if(selected) dc->FillRoundedRectangle({chk,2*dpi*scala,2*dpi*scala},pennello);
            else dc->DrawRoundedRectangle({chk,2*dpi*scala,2*dpi*scala},pennello,1*dpi*scala);
        }
        if(pinnedControls) {
            const bool fissata=std::find(lPinnedManual.begin(),lPinnedManual.end(),app.avvio)!=lPinnedManual.end();
            ID2D1Bitmap1* iconaPin=fissata?lPinPieno:lPinContorno;
            if(iconaPin) {
                const float px=base.right-24*dpi, py=base.top+27*dpi, lato=14*dpi;
                const D2D1_RECT_F pinBox=rct({px-lato*0.5f,py-lato*0.5f,px+lato*0.5f,py+lato*0.5f});
                dc->DrawBitmap(iconaPin,pinBox,1.0f,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            }
        }
    };

    if(lModalita==0) {
        // Libreria app a fisarmonica, con cartelle personalizzate persistenti.
        pennello->SetColor(tenue);
        scrivi(testi::t(L"CARTELLE LIBRERIA APP",L"APP LIBRARY FOLDERS"),fmtTitolo,{rx,84*dpi,rr-174*dpi,116*dpi});
        const D2D1_RECT_F add={rr-166*dpi,85*dpi,rr-91*dpi,115*dpi};
        const D2D1_RECT_F all={rr-84*dpi,85*dpi,rr,115*dpi};
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.07f):D2D1::ColorF(1,1,1,0.68f));
        dc->FillRoundedRectangle({add,8*dpi,8*dpi},pennello);
        dc->FillRoundedRectangle({all,8*dpi,8*dpi},pennello);
        pennello->SetColor(blu);
        scrivi(L"+",fmtTitolo,{add.left+5*dpi,add.top,add.left+23*dpi,add.bottom});
        pennello->SetColor(ink);
        scrivi(testi::t(L"Cartella",L"Folder"),fmtNome,{add.left+23*dpi,add.top,add.right-3*dpi,add.bottom});
        scrivi(testi::t(L"Tutte le app",L"All Apps"),fmtNome,{all.left+5*dpi,all.top,all.right-4*dpi,all.bottom});

        dc->PushAxisAlignedClip({rx,116*dpi,rr,lFooterTop()-8*dpi},D2D1_ANTIALIAS_MODE_ALIASED);
        for(int k=0;k<(int)lCartelle.size();++k) {
            const float fy=lCartellaY(k);
            const bool aperta=k==lCartellaAperta;
            const float h=aperta?114*dpi:36*dpi;
            if(fy+h<116*dpi || fy>lFooterTop()-8*dpi) continue;
            const D2D1_RECT_F fr={rx,fy,rr,fy+h};
            pennello->SetColor(aperta?(scuro?D2D1::ColorF(1,1,1,0.055f):D2D1::ColorF(1,1,1,0.78f))
                                       :(scuro?D2D1::ColorF(1,1,1,0.035f):D2D1::ColorF(1,1,1,0.50f)));
            dc->FillRoundedRectangle({fr,14*dpi,14*dpi},pennello);
            if(k==lSopraCartella) {
                const D2D1_RECT_F headerName={fr.left+8*dpi,fy,fr.right-120*dpi,fy+36*dpi};
                pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.095f):D2D1::ColorF(0,0,0,0.075f));
                dc->FillRoundedRectangle({headerName,9*dpi,9*dpi},pennello);
            }
            if(aperta) {
                pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.09f):D2D1::ColorF(0,0,0,0.045f));
                dc->DrawLine({fr.left,fy+36*dpi},{fr.right,fy+36*dpi},pennello,1*dpi);
            }
            // Cartelle senza icone: nome sempre allineato a sinistra come nella reference.
            pennello->SetColor(ink);
            scrivi(lCartelle[k].nome,fmtVoce,{fr.left+14*dpi,fy+1*dpi,fr.right-126*dpi,fy+35*dpi});
            const auto apps=lAppCategoria(k);
            pennello->SetColor(tenue);
            scrivi(std::to_wstring(apps.size()),fmtNome,{fr.right-52*dpi,fy+1*dpi,fr.right-32*dpi,fy+35*dpi});
            pennello->SetColor(ink);
            disegnaFreccia(fr.right-16*dpi,fy+18*dpi,aperta?3.141592653589793:0.0,9*dpi);
            if(aperta) {
                for(int j=0;j<(int)apps.size() && j<6;++j) {
                    const int col=j%2,row=j/2,appIndex=apps[j];
                    const float cw=(rr-rx-22*dpi)*0.5f;
                    const float xx=rx+10*dpi+col*cw,yy=fy+39*dpi+row*25*dpi;
                    const bool hoverApp=appIndex==lSopraAppCartella;
                    const bool hoverRemove=appIndex==lSopraRimuoviAppCartella;
                    const D2D1_RECT_F appRow={xx,yy+1*dpi,xx+cw,yy+24*dpi};
                    if(hoverApp) {
                        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.085f):D2D1::ColorF(0,0,0,0.065f));
                        dc->FillRoundedRectangle({appRow,6*dpi,6*dpi},pennello);
                    }
                    const D2D1_RECT_F ir={xx+2*dpi,yy+4*dpi,xx+18*dpi,yy+20*dpi};
                    drawAppIcon(appIndex,ir);
                    pennello->SetColor(ink);
                    scrivi(tutteApp[appIndex].nome,fmtNome,{xx+25*dpi,yy,xx+cw-27*dpi,yy+24*dpi});
                    const D2D1_RECT_F removeBox={xx+cw-22*dpi,yy+3*dpi,xx+cw-4*dpi,yy+21*dpi};
                    if(hoverRemove) {
                        pennello->SetColor(scuro?D2D1::ColorF(0.96f,0.20f,0.24f,0.19f):D2D1::ColorF(0.88f,0.10f,0.14f,0.12f));
                        dc->FillRoundedRectangle({removeBox,5*dpi,5*dpi},pennello);
                    }
                    pennello->SetColor(hoverRemove?D2D1::ColorF(0.96f,0.24f,0.28f,1.0f)
                                                   :D2D1::ColorF(ink.r,ink.g,ink.b,0.58f));
                    dc->DrawLine({removeBox.left+5*dpi,removeBox.top+5*dpi},{removeBox.right-5*dpi,removeBox.bottom-5*dpi},pennello,1.45f*dpi);
                    dc->DrawLine({removeBox.right-5*dpi,removeBox.top+5*dpi},{removeBox.left+5*dpi,removeBox.bottom-5*dpi},pennello,1.45f*dpi);
                }
            }
        }
        dc->PopAxisAlignedClip();
    }
    else if(lModalita==1) {
        pennello->SetColor(ink);
        scrivi(std::wstring(testi::t(L"TUTTE LE APP INSTALLATE",L"ALL INSTALLED APPS"))+L" ("+std::to_wstring(tutteApp.size())+L")",
               fmtTitolo,{rx,90*dpi,rr-96*dpi,122*dpi});
        const D2D1_RECT_F folders={rr-84*dpi,85*dpi,rr,115*dpi};
        pennello->SetColor(scuro?D2D1::ColorF(0.10f,0.35f,0.55f,0.34f):D2D1::ColorF(0.10f,0.48f,0.78f,0.12f));
        dc->FillRoundedRectangle({folders,8*dpi,8*dpi},pennello);
        pennello->SetColor(blu);
        scrivi(std::wstring(L"▱  ")+testi::t(L"Cartelle",L"Folders"),fmtNome,{folders.left+5*dpi,folders.top,folders.right-4*dpi,folders.bottom});

        dc->PushAxisAlignedClip({rx,136*dpi,rr,lFooterTop()-8*dpi},D2D1_ANTIALIAS_MODE_ALIASED);
        if(lVistaLista) {
            wchar_t precedente=0;
            for(int i=0;i<(int)filtrate.size();++i) {
                const D2D1_RECT_F riga=lCella(i);
                if(riga.bottom<136*dpi || riga.top>lFooterTop()-8*dpi) continue;
                const AppL& app=tutteApp[filtrate[i]];
                const wchar_t lettera=app.nome.empty()?L'#':(wchar_t)towupper(app.nome[0]);
                if(i==0 || lettera!=precedente) {
                    const float hy=riga.top-22*dpi;
                    if(hy+19*dpi>=136*dpi) {
                        pennello->SetColor(tenue);
                        scrivi(std::wstring(1,lettera),fmtVoce,{rx+4*dpi,hy,rx+27*dpi,hy+18*dpi});
                    }
                    precedente=lettera;
                }
                drawAppLibraryRow(filtrate[i],{riga.left,riga.top+1*dpi,riga.right-1*dpi,riga.bottom-2*dpi},false,true,i==lSopra);
            }
        } else {
            for(int i=0;i<(int)filtrate.size();++i) {
                const D2D1_RECT_F riga=lCella(i);
                if(riga.bottom<136*dpi || riga.top>lFooterTop()-8*dpi) continue;
                const int index=filtrate[i];
                const bool hover=i==lSopra;
                const float scala=hover?(float)lScalaAllHover:1.0f,cx=(riga.left+riga.right)*0.5f,cy=(riga.top+riga.bottom)*0.5f;
                const D2D1_RECT_F tile=scaleRect({riga.left+(riga.right-riga.left-42*dpi)*0.5f,riga.top+8*dpi,
                                                  riga.left+(riga.right-riga.left+42*dpi)*0.5f,riga.top+50*dpi},cx,cy,scala);
                const D2D1_RECT_F area=scaleRect(riga,cx,cy,scala);
                pennello->SetColor(hover?(scuro?D2D1::ColorF(1,1,1,0.095f):D2D1::ColorF(0,0,0,0.075f))
                                        :(scuro?D2D1::ColorF(1,1,1,0.055f):D2D1::ColorF(0,0,0,0.035f)));
                dc->FillRoundedRectangle({area,10*dpi*scala,10*dpi*scala},pennello);
                drawAppIcon(index,tile);
                pennello->SetColor(ink);
                scrivi(tutteApp[index].nome,fmtNome,scaleRect({riga.left+4*dpi,tile.bottom+4*dpi,riga.right-4*dpi,riga.bottom-3*dpi},cx,cy,scala));
            }
        }
        if(filtrate.empty()) {
            pennello->SetColor(tenue);
            scrivi(testi::t(L"Nessuna app disponibile",L"No installed apps found"),fmtNome,{rx+8*dpi,150*dpi,rr-8*dpi,184*dpi});
        }
        dc->PopAxisAlignedClip();
    }
    else if(lModalita==2 && lCartellaGestita>=0 && lCartellaGestita<(int)lCartelle.size()) {
        const float top=122*dpi,bottom=lFooterTop()-16*dpi;
        const D2D1_RECT_F pane={rx,top,rr,bottom};
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.045f):D2D1::ColorF(1,1,1,0.66f));
        dc->FillRoundedRectangle({pane,16*dpi,16*dpi},pennello);
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.10f):D2D1::ColorF(0,0,0,0.08f));
        dc->DrawRoundedRectangle({pane,16*dpi,16*dpi},pennello,1*dpi);
        const int count=(int)lAppCategoria(lCartellaGestita).size();
        pennello->SetColor(tenue);
        scrivi(testi::t(L"GESTISCI APP CARTELLA",L"MANAGE FOLDER APPS"),fmtTitolo,{rx,90*dpi,rr-92*dpi,122*dpi});
        const D2D1_RECT_F back={rr-84*dpi,85*dpi,rr,115*dpi};
        pennello->SetColor(scuro?D2D1::ColorF(0.10f,0.35f,0.55f,0.34f):D2D1::ColorF(0.10f,0.48f,0.78f,0.12f));
        dc->FillRoundedRectangle({back,8*dpi,8*dpi},pennello);
        pennello->SetColor(blu);
        scrivi(std::wstring(L"▱  ")+testi::t(L"Cartelle",L"Folders"),fmtNome,{back.left+5*dpi,back.top,back.right-4*dpi,back.bottom});
        pennello->SetColor(ink);
        scrivi(lCartelle[lCartellaGestita].nome,fmtVoce,{pane.left+14*dpi,pane.top+5*dpi,pane.right-100*dpi,pane.top+36*dpi});
        pennello->SetColor(tenue);
        scrivi(std::to_wstring(count)+testi::t(L" selezionate",L" selected"),fmtNome,{pane.right-92*dpi,pane.top+5*dpi,pane.right-14*dpi,pane.top+36*dpi});
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.09f):D2D1::ColorF(0,0,0,0.065f));
        dc->DrawLine({pane.left+12*dpi,pane.top+42*dpi},{pane.right-12*dpi,pane.top+42*dpi},pennello,1*dpi);

        dc->PushAxisAlignedClip({pane.left+7*dpi,pane.top+48*dpi,pane.right-7*dpi,bottom-57*dpi},D2D1_ANTIALIAS_MODE_ALIASED);
        const float listLeft=dx+28*dpi,listRight=rr-14*dpi,cw=(listRight-listLeft-10*dpi)*0.5f,rowH=35*dpi;
        for(int i=0;i<(int)tutteApp.size();++i) {
            const int col=i%2,row=i/2;
            const float x=listLeft+col*(cw+10*dpi),y=180*dpi+row*rowH-(float)lScrollEditor;
            const D2D1_RECT_F item={x,y,x+cw,y+31*dpi};
            if(item.bottom< pane.top+48*dpi || item.top>bottom-57*dpi) continue;
            const int catApp=lCategoriaApp(tutteApp[i]);
            const bool selected=catApp==lCartellaGestita;
            pennello->SetColor(selected?(scuro?D2D1::ColorF(0.05f,0.31f,0.49f,0.58f):D2D1::ColorF(0.04f,0.49f,0.78f,0.16f))
                                      :(scuro?D2D1::ColorF(1,1,1,0.025f):D2D1::ColorF(0,0,0,0.025f)));
            dc->FillRoundedRectangle({item,8*dpi,8*dpi},pennello);
            const D2D1_RECT_F chk={item.left+9*dpi,item.top+9*dpi,item.left+19*dpi,item.top+19*dpi};
            pennello->SetColor(selected?blu:tenue);
            if(selected) dc->FillRoundedRectangle({chk,2*dpi,2*dpi},pennello);
            else dc->DrawRoundedRectangle({chk,2*dpi,2*dpi},pennello,1*dpi);
            pennello->SetColor(ink);
            scrivi(tutteApp[i].nome,fmtNome,{item.left+27*dpi,item.top,item.right-5*dpi,item.bottom});
        }
        dc->PopAxisAlignedClip();
        const float by=bottom-48*dpi;
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.07f):D2D1::ColorF(0,0,0,0.035f));
        dc->FillRoundedRectangle({{pane.left+12*dpi,by,pane.right-12*dpi,bottom-10*dpi},10*dpi,10*dpi},pennello);
        pennello->SetColor(blu);
        scrivi(testi::t(L"Fatto",L"Done"),fmtTitolo,{pane.right-86*dpi,by,pane.right-14*dpi,bottom-10*dpi});
    }
    else if(lModalita==3 || lModalita==5) {
        const bool rinomina=lModalita==5;
        const float top=122*dpi,bottom=lFooterTop()-16*dpi;
        const D2D1_RECT_F pane={rx,top,rr,bottom};
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.045f):D2D1::ColorF(1,1,1,0.66f));
        dc->FillRoundedRectangle({pane,16*dpi,16*dpi},pennello);
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.10f):D2D1::ColorF(0,0,0,0.08f));
        dc->DrawRoundedRectangle({pane,16*dpi,16*dpi},pennello,1*dpi);
        pennello->SetColor(tenue);
        scrivi(rinomina?testi::t(L"RINOMINA CARTELLA",L"RENAME FOLDER")
                       :testi::t(L"NUOVA CARTELLA LIBRERIA",L"NEW LIBRARY FOLDER"),
               fmtTitolo,{rx,90*dpi,rr-92*dpi,122*dpi});
        const D2D1_RECT_F back={rr-84*dpi,85*dpi,rr,115*dpi};
        pennello->SetColor(scuro?D2D1::ColorF(0.10f,0.35f,0.55f,0.34f):D2D1::ColorF(0.10f,0.48f,0.78f,0.12f));
        dc->FillRoundedRectangle({back,8*dpi,8*dpi},pennello);
        pennello->SetColor(blu);
        scrivi(std::wstring(L"‹  ")+testi::t(L"Indietro",L"Back"),fmtVoce,{back.left+5*dpi,back.top,back.right-4*dpi,back.bottom});

        pennello->SetColor(tenue);
        scrivi(testi::t(L"NOME CARTELLA",L"FOLDER NAME"),fmtSotto,{pane.left+12*dpi,pane.top+9*dpi,pane.right-12*dpi,pane.top+28*dpi});
        const D2D1_RECT_F nameBox={pane.left+12*dpi,pane.top+30*dpi,pane.right-12*dpi,pane.top+62*dpi};
        pennello->SetColor(lNomeNuovoAttivo?(scuro?D2D1::ColorF(0.10f,0.34f,0.52f,0.30f):D2D1::ColorF(0.10f,0.48f,0.82f,0.10f))
                                               :(scuro?D2D1::ColorF(0.01f,0.02f,0.04f,0.44f):D2D1::ColorF(0,0,0,0.035f)));
        dc->FillRoundedRectangle({nameBox,9*dpi,9*dpi},pennello);
        pennello->SetColor(ink);
        scrivi(lNomeNuovaCartella.empty()?testi::t(L"Digita il nome della cartella...",L"Enter a folder name..."):lNomeNuovaCartella,
               fmtVoce,{nameBox.left+11*dpi,nameBox.top,nameBox.right-10*dpi,nameBox.bottom});
        if(lNomeNuovoAttivo) {
            const float tw=larghezzaTesto(lNomeNuovaCartella,fmtVoce);
            pennello->SetColor(blu);
            dc->DrawLine({nameBox.left+11*dpi+tw+2*dpi,nameBox.top+8*dpi},
                         {nameBox.left+11*dpi+tw+2*dpi,nameBox.bottom-8*dpi},pennello,1.4f*dpi);
        }

        if(!rinomina) {
            pennello->SetColor(tenue);
            scrivi(std::wstring(testi::t(L"SELEZIONA APP",L"SELECT APPS"))+L" ("+std::to_wstring(lAppNuovaCartella.size())+L")",
                   fmtSotto,{pane.left+12*dpi,pane.top+72*dpi,pane.right-12*dpi,pane.top+94*dpi});
            dc->PushAxisAlignedClip({pane.left+7*dpi,pane.top+102*dpi,pane.right-7*dpi,bottom-57*dpi},D2D1_ANTIALIAS_MODE_ALIASED);
            const float listLeft=dx+28*dpi,listRight=rr-14*dpi,cw=(listRight-listLeft-10*dpi)*0.5f,rowH=35*dpi;
            for(int i=0;i<(int)tutteApp.size();++i) {
                const int col=i%2,row=i/2;
                const float x=listLeft+col*(cw+10*dpi),y=234*dpi+row*rowH-(float)lScrollEditor;
                const D2D1_RECT_F item={x,y,x+cw,y+31*dpi};
                if(item.bottom<pane.top+102*dpi || item.top>bottom-57*dpi) continue;
                const bool selected=std::find(lAppNuovaCartella.begin(),lAppNuovaCartella.end(),tutteApp[i].avvio)!=lAppNuovaCartella.end();
                pennello->SetColor(selected?(scuro?D2D1::ColorF(0.05f,0.31f,0.49f,0.58f):D2D1::ColorF(0.04f,0.49f,0.78f,0.16f))
                                          :(scuro?D2D1::ColorF(1,1,1,0.025f):D2D1::ColorF(0,0,0,0.025f)));
                dc->FillRoundedRectangle({item,8*dpi,8*dpi},pennello);
                const D2D1_RECT_F chk={item.left+9*dpi,item.top+9*dpi,item.left+19*dpi,item.top+19*dpi};
                pennello->SetColor(selected?blu:tenue);
                if(selected) dc->FillRoundedRectangle({chk,2*dpi,2*dpi},pennello);
                else dc->DrawRoundedRectangle({chk,2*dpi,2*dpi},pennello,1*dpi);
                pennello->SetColor(ink);
                scrivi(tutteApp[i].nome,fmtVoce,{item.left+27*dpi,item.top,item.right-5*dpi,item.bottom});
            }
            dc->PopAxisAlignedClip();
        }
        const float by=bottom-48*dpi;
        const D2D1_RECT_F cancel={pane.left+12*dpi,by,pane.right-104*dpi,bottom-10*dpi};
        const D2D1_RECT_F save={pane.right-98*dpi,by,pane.right-12*dpi,bottom-10*dpi};
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.07f):D2D1::ColorF(0,0,0,0.035f));
        dc->FillRoundedRectangle({cancel,9*dpi,9*dpi},pennello);
        pennello->SetColor(tenue);
        scrivi(testi::t(L"Annulla",L"Cancel"),fmtVoce,cancel);
        pennello->SetColor(blu);
        dc->FillRoundedRectangle({save,9*dpi,9*dpi},pennello);
        pennello->SetColor(D2D1::ColorF(0.02f,0.04f,0.08f,1));
        scrivi(rinomina?testi::t(L"Salva",L"Save"):testi::t(L"Crea cartella",L"Create Folder"),fmtVoce,save);
    }
    else {
        // Vista Spotlight: risultati a schede, come nello screenshot di ricerca.
        pennello->SetColor(blu);
        scrivi(std::wstring(testi::t(L"TOP MATCHES",L"TOP MATCHES"))+L" ("+std::to_wstring(filtrate.size())+L")",
               fmtTitolo,{16*dpi,78*dpi,LW-16*dpi,108*dpi});
        if(filtrate.empty()) {
            pennello->SetColor(tenue);
            scrivi(testi::t(L"Nessuna app trovata",L"No apps found"),fmtNome,{16*dpi,128*dpi,LW-16*dpi,162*dpi});
            const D2D1_RECT_F web={16*dpi,170*dpi,std::min(LW-16*dpi,460*dpi),212*dpi};
            pennello->SetColor(scuro?D2D1::ColorF(0.18f,0.34f,0.54f,0.22f):D2D1::ColorF(0.15f,0.42f,0.82f,0.10f));
            dc->FillRoundedRectangle({web,10*dpi,10*dpi},pennello);
            pennello->SetColor(blu);
            scrivi(testi::t(L"Cerca sul Web: ",L"Search the Web for: ")+query,fmtNome,
                   {web.left+12*dpi,web.top,web.right-12*dpi,web.bottom});
        } else {
            for(int i=0;i<(int)filtrate.size() && i<8;++i) {
                const D2D1_RECT_F riga=lCella(i);
                const int index=filtrate[i],cat=lCategoriaApp(tutteApp[index]);
                pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.05f):D2D1::ColorF(1,1,1,0.62f));
                dc->FillRoundedRectangle({riga,11*dpi,11*dpi},pennello);
                const D2D1_RECT_F icon={riga.left+11*dpi,riga.top+9*dpi,riga.left+48*dpi,riga.top+46*dpi};
                drawAppIcon(index,icon);
                pennello->SetColor(ink);
                scrivi(tutteApp[index].nome,fmtNome,{riga.left+60*dpi,riga.top+7*dpi,riga.right-104*dpi,riga.top+31*dpi});
                pennello->SetColor(tenue);
                scrivi(cat>=0?lCartelle[cat].nome:testi::t(L"Non assegnata",L"Unassigned"),fmtNome,
                       {riga.left+60*dpi,riga.top+30*dpi,riga.right-104*dpi,riga.bottom-4*dpi});
                const D2D1_RECT_F launch={riga.right-96*dpi,riga.top+15*dpi,riga.right-10*dpi,riga.bottom-15*dpi};
                pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.06f):D2D1::ColorF(0,0,0,0.035f));
                dc->FillRoundedRectangle({launch,7*dpi,7*dpi},pennello);
                pennello->SetColor(tenue);
                scrivi(testi::t(L"Avvia  ↵",L"Launch  ↵"),fmtNome,launch);
            }
        }
    }

    // Footer: profilo utente allineato a sinistra; i tre simboli non hanno piastrella finche' non sono in hover.
    {
        const float top=lFooterTop();
        const D2D1_COLOR_F footerColor=scuro?D2D1::ColorF(0.015f,0.025f,0.045f,0.72f):D2D1::ColorF(0.92f,0.94f,0.97f,0.82f);
        pennello->SetColor(footerColor);
        dc->FillRoundedRectangle({{0,top,LW,LH},22*dpi,22*dpi},pennello);
        dc->FillRectangle({0,top,LW,LH-22*dpi},pennello);
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.10f):D2D1::ColorF(0,0,0,0.08f));
        dc->DrawLine({0,top},{LW,top},pennello,1*dpi);

        const D2D1_RECT_F rp=rettProfilo(),re=rettEnergia();
        const float ax=rp.left+22*dpi, ay=(rp.top+rp.bottom)*0.5f, radius=17*dpi;
        pennello->SetColor(D2D1::ColorF(0.16f,0.54f,0.96f,1));
        dc->FillEllipse({{ax,ay},radius,radius},pennello);
        if(lFotoProfilo) {
            const D2D1_RECT_F avatar={ax-radius,ay-radius,ax+radius,ay+radius};
            dc->DrawBitmap(lFotoProfilo,avatar,1.0f,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.20f):D2D1::ColorF(0,0,0,0.10f));
            dc->DrawEllipse({{ax,ay},radius,radius},pennello,0.8f*dpi);
        } else {
            wchar_t utente[128]=L""; DWORD nUtente=128;
            if(GetUserNameW(utente,&nUtente)&&utente[0]) {
                std::wstring iniziali(1,(wchar_t)towupper(utente[0]));
                for(size_t i=1;i<wcslen(utente);++i) if(utente[i]==L' '&&utente[i+1]) { iniziali+=towupper(utente[i+1]); break; }
                pennello->SetColor(D2D1::ColorF(1,1,1,1));
                scrivi(iniziali,fmtNome,{ax-radius,ay-radius,ax+radius,ay+radius});
            }
        }

        wchar_t utente[128]=L""; DWORD nUtente=128;
        std::wstring nome=L"User";
        if(GetUserNameW(utente,&nUtente)&&utente[0]) nome=utente;
        pennello->SetColor(ink);
        scrivi(nome,fmtVoce,{rp.left+48*dpi,rp.top+1*dpi,rp.right-4*dpi,rp.top+23*dpi});
        pennello->SetColor(tenue);
        scrivi(testi::t(L"Spazio di lavoro locale",L"Local Workspace"),fmtSotto,
               {rp.left+48*dpi,rp.top+21*dpi,rp.right-4*dpi,rp.bottom});

        const D2D1_RECT_F files={LW-184*dpi,top+6*dpi,LW-140*dpi,top+46*dpi};
        const D2D1_RECT_F settings={LW-128*dpi,top+6*dpi,LW-84*dpi,top+46*dpi};
        const D2D1_RECT_F power=re;
        auto hoverTile=[&](const D2D1_RECT_F& r,bool hover) {
            if(!hover) return;
            pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.12f):D2D1::ColorF(0,0,0,0.075f));
            dc->FillRoundedRectangle({r,9*dpi,9*dpi},pennello);
        };
        hoverTile(files,lSopraEsplora);
        hoverTile(settings,lSopraImpostazioni);
        hoverTile(power,lSopraEnergia);

        // File Explorer: sagoma cartella, disegnata senza font-glyph dipendenti dal sistema.
        const float fx=(files.left+files.right)*0.5f, fy=(files.top+files.bottom)*0.5f;
        pennello->SetColor(ink);
        const D2D1_POINT_2F folderPts[]={{fx-12*dpi,fy-6*dpi},{fx-4*dpi,fy-6*dpi},{fx-1*dpi,fy-2*dpi},
            {fx+12*dpi,fy-2*dpi},{fx+12*dpi,fy+8*dpi},{fx-12*dpi,fy+8*dpi},{fx-12*dpi,fy-6*dpi}};
        for(int i=0;i<6;++i) dc->DrawLine(folderPts[i],folderPts[i+1],pennello,1.8f*dpi);
        dc->DrawLine({fx-12*dpi,fy-2*dpi},{fx+12*dpi,fy-2*dpi},pennello,1.3f*dpi);

        // Settings: gear con otto denti e anello centrale.
        const float sx=(settings.left+settings.right)*0.5f, sy=(settings.top+settings.bottom)*0.5f;
        dc->DrawEllipse({{sx,sy},7.5f*dpi,7.5f*dpi},pennello,2.1f*dpi);
        dc->DrawEllipse({{sx,sy},2.7f*dpi,2.7f*dpi},pennello,1.8f*dpi);
        for(int i=0;i<8;++i) {
            const float a=i*0.785398163f, co=std::cos(a),si=std::sin(a);
            dc->DrawLine({sx+co*8.2f*dpi,sy+si*8.2f*dpi},{sx+co*11.0f*dpi,sy+si*11.0f*dpi},pennello,2.1f*dpi);
        }

        // Power: cerchio aperto in alto con stelo verticale.
        const float px=(power.left+power.right)*0.5f, py=(power.top+power.bottom)*0.5f;
        dc->DrawEllipse({{px,py+1*dpi},9.5f*dpi,9.5f*dpi},pennello,1.8f*dpi);
        dc->DrawLine({px,py-11*dpi},{px,py+1*dpi},pennello,2.0f*dpi);
    }

    // Native-looking FluidWin folder menu: matching dark/light surface, rounded edge and restrained hover.
    if(lCartellaMenuAperto && lCartellaMenuIndice>=0 && lCartellaMenuIndice<(int)lCartelle.size()) {
        const float mx=lCartellaMenuX,my=lCartellaMenuY,mw=184*dpi,mh=132*dpi;
        const D2D1_RECT_F panel={mx,my,mx+mw,my+mh};
        pennello->SetColor(scuro?D2D1::ColorF(0.055f,0.065f,0.085f,0.985f):D2D1::ColorF(0.96f,0.97f,0.985f,0.985f));
        dc->FillRoundedRectangle({panel,12*dpi,12*dpi},pennello);
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.13f):D2D1::ColorF(0.10f,0.14f,0.20f,0.12f));
        dc->DrawRoundedRectangle({panel,12*dpi,12*dpi},pennello,1*dpi);
        pennello->SetColor(tenue);
        scrivi(testi::t(L"CARTELLA",L"FOLDER"),fmtSotto,{mx+12*dpi,my+4*dpi,mx+mw-12*dpi,my+28*dpi});
        pennello->SetColor(ink);
        scrivi(lCartelle[lCartellaMenuIndice].nome,fmtVoce,{mx+12*dpi,my+20*dpi,mx+mw-12*dpi,my+42*dpi});
        pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.10f):D2D1::ColorF(0,0,0,0.08f));
        dc->DrawLine({mx+10*dpi,my+44*dpi},{mx+mw-10*dpi,my+44*dpi},pennello,1*dpi);
        const wchar_t* etichette[3]={
            testi::t(L"Rinomina",L"Rename"),
            testi::t(L"Gestisci app",L"Manage apps"),
            testi::t(L"Rimuovi",L"Remove")
        };
        const wchar_t* simboli[3]={L"✎",L"☷",L"−"};
        for(int k=0;k<3;++k) {
            const D2D1_RECT_F row={mx+7*dpi,my+(48+k*27)*dpi,mx+mw-7*dpi,my+(74+k*27)*dpi};
            if(lCartellaMenuHover==k) {
                pennello->SetColor(scuro?D2D1::ColorF(1,1,1,0.10f):D2D1::ColorF(0.05f,0.20f,0.38f,0.08f));
                dc->FillRoundedRectangle({row,7*dpi,7*dpi},pennello);
            }
            pennello->SetColor(k==2?D2D1::ColorF(0.92f,0.30f,0.34f,1.0f):ink);
            scrivi(simboli[k],fmtVoce,{row.left+8*dpi,row.top,row.left+30*dpi,row.bottom});
            scrivi(etichette[k],fmtVoce,{row.left+36*dpi,row.top,row.right-7*dpi,row.bottom});
        }
    }

    // barra di scorrimento sottile, solo se serve; col mouse sopra (o trascinandola) si allarga, come su macOS
    if ((lModalita == 1 || lModalita == 4) && lScrollMax() > 0) {
        const D2D1_RECT_F b = lCursoreBarra();
        const bool larga = lSopraBarra || lTrascinoBarra;
        const float w = (larga ? 7.0f : 3.0f) * dpi, x1 = LW - 6 * dpi;
        pennello->SetColor({ink.r, ink.g, ink.b, larga ? 0.42f : 0.25f});
        dc->FillRoundedRectangle({{x1 - w, b.top, x1, b.bottom}, w / 2, w / 2}, pennello);
    }
    controllaGpu(dc->EndDraw());
    dc->SetTarget(bersaglio);
    lAnim = anim; lDirty = false;
    return true;
}

inline LRESULT CALLBACK Motore::procLauncher(HWND h, UINT m, WPARAM w, LPARAM l) {
    Motore* d = gIstanza();
    if (!d) return DefWindowProcW(h, m, w, l);
    const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
    switch (m) {
        case WM_LBUTTONDOWN: {
            // barra di scorrimento: preso il cursore lo si trascina; un clic sul binario salta di una pagina
            const D2D1_RECT_F b = d->lRettBarra();
            if ((d->lModalita == 1 || d->lModalita == 4) && d->lScrollMax() > 0 && x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
                const D2D1_RECT_F c = d->lCursoreBarra();
                if (y < c.top || y >= c.bottom) {
                    const double pagina = (b.bottom - b.top) * 0.9;
                    d->lScrollObj = std::clamp(d->lScrollObj + (y < c.top ? -pagina : pagina), 0.0, d->lScrollMax()); d->lSelTastiera = false;
                } else {
                    d->lTrascinoBarra = true; d->lPresaY = y; d->lPresaScroll = d->lScroll; d->lSelTastiera = false;
                    SetCapture(h);
                }
                d->lDirty = true;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if(!d->lMouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm),TME_LEAVE,h,0}; TrackMouseEvent(&tm); d->lMouseDentro=true; }
            if(d->lCartellaMenuAperto) {
                int hover=-1;
                const float mx=d->lCartellaMenuX,my=d->lCartellaMenuY;
                if(x>=mx+7*d->dpi && x<mx+177*d->dpi && y>=my+48*d->dpi && y<my+129*d->dpi)
                    hover=(int)((y-(my+48*d->dpi))/(27*d->dpi));
                if(hover!=d->lCartellaMenuHover) { d->lCartellaMenuHover=hover; d->lDirty=true; }
                return 0;
            }
            if(d->lTrascinoBarra) {
                const double alt=d->lGrigliaBottom()-d->lGrigliaTop(),c=d->lCursoreBarra().bottom-d->lCursoreBarra().top;
                const double corsa=std::max(1.0,alt-c);
                d->lScrollObj=d->lScroll=std::clamp(d->lPresaScroll+(y-d->lPresaY)*d->lScrollMax()/corsa,0.0,d->lScrollMax());
                d->lDirty=true; return 0;
            }
            const D2D1_RECT_F b=d->lRettBarra();
            const bool sb=(d->lModalita==1 || d->lModalita==4) && d->lScrollMax()>0 &&
                          x>=b.left && x<b.right && y>=b.top && y<b.bottom;
            const int s=(d->lModalita==1 || d->lModalita==4)?d->lCellaIn(x,y):-1;
            const int pin=d->lPinnedIn(x,y);
            const int cartella=d->lModalita==0?d->lCartellaNomeIn(x,y):-1;
            const int appCartella=d->lModalita==0?d->lAppCartellaIn(x,y):-1;
            const int rimuoviApp=d->lModalita==0?d->lAppCartellaRimuoviIn(x,y):-1;
            int azione=-1;
            if(d->lModalita!=4 && x>=16*d->dpi && x<d->lDividerX()-16*d->dpi) {
                const float ay=d->LH-256*d->dpi;
                for(int k=0;k<4;++k) {
                    const float top=ay+(28.0f+k*36.0f)*d->dpi;
                    if(y>=top && y<top+30*d->dpi) { azione=k; break; }
                }
            }
            const D2D1_RECT_F rp=d->rettProfilo(),re=d->rettEnergia();
            const bool profilo=x>=rp.left && x<rp.right && y>=rp.top && y<rp.bottom;
            const bool energia=x>=re.left && x<re.right && y>=re.top && y<re.bottom;
            const float ft=d->lFooterTop();
            const bool explorer=x>=d->LW-184*d->dpi && x<d->LW-140*d->dpi && y>=ft+6*d->dpi && y<ft+46*d->dpi;
            const bool settings=x>=d->LW-128*d->dpi && x<d->LW-84*d->dpi && y>=ft+6*d->dpi && y<ft+46*d->dpi;
            if(sb!=d->lSopraBarra || s!=d->lSopra || pin!=d->lSopraPinned || cartella!=d->lSopraCartella ||
               appCartella!=d->lSopraAppCartella || rimuoviApp!=d->lSopraRimuoviAppCartella || azione!=d->lSopraAzione ||
               profilo!=d->lSopraProfilo || energia!=d->lSopraEnergia ||
               explorer!=d->lSopraEsplora || settings!=d->lSopraImpostazioni) {
                d->lSopraBarra=sb; d->lSopra=s; d->lSopraPinned=pin; d->lSopraCartella=cartella;
                d->lSopraAppCartella=appCartella; d->lSopraRimuoviAppCartella=rimuoviApp; d->lSopraAzione=azione;
                d->lSopraProfilo=profilo; d->lSopraEnergia=energia;
                d->lSopraEsplora=explorer; d->lSopraImpostazioni=settings;
                d->lDirty=true;
            }
            return 0;
        }
        case WM_MOUSELEAVE: d->lMouseDentro=false; d->lSopra=-1; d->lSopraPinned=-1; d->lSopraCartella=-1; d->lSopraAppCartella=-1; d->lSopraRimuoviAppCartella=-1; d->lSopraAzione=-1; d->lSopraVista=false; d->lSopraProfilo=false; d->lSopraFile=-1; d->lSopraEnergia=false; d->lSopraEsplora=false; d->lSopraImpostazioni=false; d->lSopraBarra=false; d->lCartellaMenuHover=-1; d->lDirty=true; return 0;
        case WM_TIMER:
            if (w == TIMER_FILE_L) { KillTimer(h, TIMER_FILE_L); d->cercaFileLauncher(); return 0; }
            break;
        case WM_LBUTTONUP: {
            if(d->lTrascinoBarra) { d->lTrascinoBarra=false; ReleaseCapture(); d->lDirty=true; return 0; }
            { const D2D1_RECT_F b=d->lRettBarra();
              if((d->lModalita==1 || d->lModalita==4) && d->lScrollMax()>0 &&
                 x>=b.left && x<b.right && y>=b.top && y<b.bottom) return 0; }
            if(d->lCartellaMenuAperto) {
                int azione=-1;
                const float mx=d->lCartellaMenuX,my=d->lCartellaMenuY;
                if(x>=mx+7*d->dpi && x<mx+177*d->dpi && y>=my+48*d->dpi && y<my+129*d->dpi)
                    azione=(int)((y-(my+48*d->dpi))/(27*d->dpi));
                const int cartella=d->lCartellaMenuIndice;
                d->lCartellaMenuAperto=false;
                d->lCartellaMenuHover=-1;
                d->lCartellaMenuIndice=-1;
                if(azione==0) d->apriRinominaCartella(cartella);
                else if(azione==1) d->apriGestisciCartella(cartella);
                else if(azione==2) d->rimuoviCartella(cartella);
                else d->lDirty=true;
                return 0;
            }
            if(y>d->LH) { d->chiudiLauncher(); return 0; }
            if(d->lSopraEnergia) { POINT p{(LONG)x,(LONG)y}; ClientToScreen(h,&p); d->menuEnergia(p); return 0; }
            if(d->lSopraProfilo) {
                d->primaDelLauncher=nullptr; d->chiudiLauncher();
                ShellExecuteW(nullptr,L"open",L"ms-settings:yourinfo",nullptr,nullptr,SW_SHOWNORMAL);
                return 0;
            }

            const float paneRight=d->LW-14*d->dpi;
            const float footer=d->lFooterTop();
            // Footer icons are deliberately unboxed until hover.
            if(d->lSopraEsplora) {
                d->primaDelLauncher=nullptr; d->chiudiLauncher();
                ShellExecuteW(nullptr,L"open",L"explorer.exe",nullptr,nullptr,SW_SHOWNORMAL); return 0;
            }
            if(d->lSopraImpostazioni) {
                d->primaDelLauncher=nullptr; d->chiudiLauncher();
                if(d->impostazioni) d->impostazioni(nullptr);
                else ShellExecuteW(nullptr,L"open",L"ms-settings:",nullptr,nullptr,SW_SHOWNORMAL);
                return 0;
            }

            // Torna alla vista folders dalle pagine di libreria secondarie.
            if((d->lModalita==1 || d->lModalita==2 || d->lModalita==3 || d->lModalita==5) &&
               x>=paneRight-84*d->dpi && x<paneRight && y>=85*d->dpi && y<115*d->dpi) {
                d->lModalita=0; d->lNomeNuovoAttivo=false; d->lCartellaRinomina=-1;
                d->lNomeNuovaCartella.clear(); d->lScrollEditor=0; d->lDirty=true; return 0;
            }

            // Gestione appartenenza app a una cartella.
            if(d->lModalita==2) {
                const float bottom=d->lFooterTop()-16*d->dpi,by=bottom-48*d->dpi;
                if(x>=paneRight-86*d->dpi && x<paneRight-8*d->dpi && y>=by && y<bottom-10*d->dpi) {
                    d->lModalita=0; d->lScrollEditor=0; d->lDirty=true; return 0;
                }
                const int app=d->lGestisciAppIn(x,y);
                if(app>=0) { d->toggleAppCartella(app,d->lCartellaGestita); return 0; }
                return 0;
            }

            // Crea/Rinomina: campo testo, elenco app solo durante la creazione.
            if(d->lModalita==3 || d->lModalita==5) {
                const bool rinomina=d->lModalita==5;
                const float left=d->lDividerX()+14*d->dpi,right=d->LW-14*d->dpi;
                const float bottom=d->lFooterTop()-16*d->dpi,by=bottom-48*d->dpi;
                const D2D1_RECT_F nameBox={left+12*d->dpi,152*d->dpi,right-12*d->dpi,184*d->dpi};
                if(x>=nameBox.left && x<nameBox.right && y>=nameBox.top && y<nameBox.bottom) {
                    d->lNomeNuovoAttivo=true; d->lDirty=true; return 0;
                }
                d->lNomeNuovoAttivo=false;
                if(!rinomina) {
                    const int app=d->lNuovaAppIn(x,y);
                    if(app>=0) {
                        const std::wstring& path=d->tutteApp[app].avvio;
                        auto it=std::find(d->lAppNuovaCartella.begin(),d->lAppNuovaCartella.end(),path);
                        if(it==d->lAppNuovaCartella.end()) d->lAppNuovaCartella.push_back(path);
                        else d->lAppNuovaCartella.erase(it);
                        d->lDirty=true; return 0;
                    }
                }
                const D2D1_RECT_F cancel={left+12*d->dpi,by,right-104*d->dpi,bottom-10*d->dpi};
                const D2D1_RECT_F save={right-98*d->dpi,by,right-12*d->dpi,bottom-10*d->dpi};
                if(x>=cancel.left && x<cancel.right && y>=cancel.top && y<cancel.bottom) {
                    d->lModalita=0; d->lCartellaRinomina=-1; d->lNomeNuovaCartella.clear();
                    d->lAppNuovaCartella.clear(); d->lScrollEditor=0; d->lDirty=true; return 0;
                }
                if(x>=save.left && x<save.right && y>=save.top && y<save.bottom) {
                    if(rinomina) d->salvaRinominaCartella();
                    else d->creaCartella();
                    return 0;
                }
                return 0;
            }

            // Pulsante + Folder e All Apps.
            if(d->lModalita==0 && y>=85*d->dpi && y<115*d->dpi) {
                if(x>=paneRight-84*d->dpi && x<paneRight) {
                    d->lModalita=1; d->lScroll=d->lScrollObj=0; d->lSel=0; d->lDirty=true; return 0;
                }
                if(x>=paneRight-166*d->dpi && x<paneRight-91*d->dpi) {
                    d->lNomeNuovaCartella.clear(); d->lAppNuovaCartella.clear();
                    d->lScrollEditor=0; d->lModalita=3; d->lNomeNuovoAttivo=true;
                    d->lDirty=true; return 0;
                }
            }

            if(d->lModalita==0) {
                // Left click anywhere on a folder row expands/collapses it; context menu is right-click only.
                const int folder=d->lCartellaIn(x,y);
                if(folder>=0) {
                    d->lCartellaAperta=(d->lCartellaAperta==folder?-1:folder);
                    d->lDirty=true; return 0;
                }
                const int removeApp=d->lAppCartellaRimuoviIn(x,y);
                if(removeApp>=0) {
                    d->rimuoviAppDaCartella(removeApp,d->lCartellaAperta);
                    return 0;
                }
            }

            // Four quick actions use the reserved area below the six pinned apps.
            if(d->lModalita!=4 && x>=16*d->dpi && x<d->lDividerX()-16*d->dpi) {
                const float ay=d->LH-256*d->dpi;
                const int action=(int)((y-(ay+28*d->dpi))/(36*d->dpi));
                if(y>=ay+28*d->dpi && y<ay+28*d->dpi+4*36*d->dpi && action>=0 && action<4) {
                    d->primaDelLauncher=nullptr; d->chiudiLauncher();
                    if(action==0) ShellExecuteW(nullptr,L"open",L"wt.exe",nullptr,nullptr,SW_SHOWNORMAL);
                    else if(action==1) ShellExecuteW(nullptr,L"open",L"ms-screenclip:",nullptr,nullptr,SW_SHOWNORMAL);
                    else if(action==2) ShellExecuteW(nullptr,L"open",L"taskmgr.exe",nullptr,nullptr,SW_SHOWNORMAL);
                    else {
                        PWSTR downloads=nullptr;
                        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads,0,nullptr,&downloads)) && downloads) {
                            ShellExecuteW(nullptr,L"open",L"explorer.exe",downloads,nullptr,SW_SHOWNORMAL);
                            CoTaskMemFree(downloads);
                        } else ShellExecuteW(nullptr,L"open",L"shell:Downloads",nullptr,nullptr,SW_SHOWNORMAL);
                    }
                    return 0;
                }
            }

            // Search clear / Web search.
            if(!d->query.empty() && x>=d->LW-107*d->dpi && x<d->LW-82*d->dpi && y>=14*d->dpi && y<54*d->dpi) {
                d->query.clear(); d->filtra(); d->lModalita=0;
                d->lSelFile=-1; d->lScroll=d->lScrollObj=0; d->lDirty=true; return 0;
            }
            if(d->lModalita==4 && d->filtrate.empty() && !d->query.empty() &&
               x>=16*d->dpi && x<std::min(d->LW-16*d->dpi,460*d->dpi) &&
               y>=170*d->dpi && y<212*d->dpi) {
                const std::wstring url=L"https://www.google.com/search?q="+codificaQueryWeb(d->query);
                d->primaDelLauncher=nullptr; d->chiudiLauncher();
                ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL); return 0;
            }

            const int pin=d->lPinnedIn(x,y);
            if(pin>=0 && pin<(int)d->lPinned.size()) { d->avviaAppDiretto(d->lPinned[pin]); return 0; }
            const int i=(d->lModalita==1 || d->lModalita==4)?d->lCellaIn(x,y):-1;
            if(i>=0) {
                const D2D1_RECT_F riga=d->lCella(i);
                const int appIndex=d->filtrate[i];
                if(d->lModalita==1 && d->lVistaLista && x>=riga.right-42*d->dpi)
                    d->togglePinned(appIndex);
                else d->avviaApp(i);
                return 0;
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            if(d->lModalita==0) {
                const int folder=d->lCartellaIn(x,y);
                if(folder>=0) { POINT pt{(LONG)x,(LONG)y}; ClientToScreen(h,&pt); d->apriMenuCartella(folder,pt); d->lDirty=true; return 0; }
            }
            int indiceApp=-1;
            if(d->lModalita==0) {
                const int pin=d->lPinnedIn(x,y);
                if(pin>=0 && pin<(int)d->lPinned.size()) indiceApp=d->lPinned[pin];
            } else if(d->lModalita==1 || d->lModalita==4) {
                const int cella=d->lCellaIn(x,y);
                if(cella>=0 && cella<(int)d->filtrate.size()) indiceApp=d->filtrate[cella];
            }
            if(indiceApp>=0) {
                POINT p{(LONG)x,(LONG)y}; ClientToScreen(h,&p);
                d->menuApp(indiceApp,p);
            } else {
                const int k=d->lModalita==1?d->lFileIn(x,y):-1;
                if(k>=0) { POINT p{(LONG)x,(LONG)y}; ClientToScreen(h,&p); d->menuFile(k,p); }
            }
            return 0;
        }
        // sottomenu del menu di Windows ("Invia a", "Apri con"...): li disegna lui
        case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
            if (d->menuShell3) { LRESULT r = 0; if (SUCCEEDED(d->menuShell3->HandleMenuMsg2(m, w, l, &r))) return r; }
            else if (d->menuShell2) { if (SUCCEEDED(d->menuShell2->HandleMenuMsg(m, w, l))) return m == WM_INITMENUPOPUP ? 0 : TRUE; }
            break;
        case WM_MOUSEWHEEL: {
            d->lSelTastiera=false;
            const double passo=GET_WHEEL_DELTA_WPARAM(w)/(double)WHEEL_DELTA;
            if(d->lModalita==0 && x>=d->lDividerX()) {
                double totale=0;
                for(int i=0;i<(int)d->lCartelle.size();++i) totale+=(i==d->lCartellaAperta?114:36)*d->dpi+7*d->dpi;
                const double max=std::max(0.0,totale-(d->lFooterTop()-8*d->dpi-122*d->dpi));
                d->lScrollCartelle=std::clamp(d->lScrollCartelle-passo*42*d->dpi,0.0,max);
            } else if(d->lModalita==2 || d->lModalita==3) {
                const double top=d->lModalita==2?180*d->dpi:234*d->dpi;
                const double bottom=d->lModalita==2?d->lFooterTop()-78*d->dpi:d->lFooterTop()-82*d->dpi;
                const double visible=std::max(0.0,bottom-top);
                const double total=std::ceil(d->tutteApp.size()/2.0)*35*d->dpi;
                const double max=std::max(0.0,total-visible);
                d->lScrollEditor=std::clamp(d->lScrollEditor-passo*42*d->dpi,0.0,max);
            } else if(d->lModalita==1 || d->lModalita==4) {
                d->lScrollObj=std::clamp(d->lScrollObj-passo*d->lCellaH()*0.9,0.0,d->lScrollMax());
            }
            d->lDirty=true; return 0;
        }
        case WM_CHAR: {
            if((d->lModalita==3 || d->lModalita==5) && d->lNomeNuovoAttivo) {
                if(w==27) { d->lNomeNuovoAttivo=false; d->lDirty=true; return 0; }
                if(w==13) { if(d->lModalita==5) d->salvaRinominaCartella(); else d->creaCartella(); return 0; }
                if(w==8) { if(!d->lNomeNuovaCartella.empty()) d->lNomeNuovaCartella.pop_back(); d->lDirty=true; return 0; }
                if(w>=32 && d->lNomeNuovaCartella.size()<48) {
                    const wchar_t ch=(wchar_t)w;
                    if(ch!=L'|' && ch!=L'/' && ch!=L'\\' && ch!=L':' && ch!=L'*' && ch!=L'?' &&
                       ch!=L'"' && ch!=L'<' && ch!=L'>') d->lNomeNuovaCartella.push_back(ch);
                    d->lDirty=true; return 0;
                }
                if(w==22 && OpenClipboard(h)) {
                    if(HANDLE g=GetClipboardData(CF_UNICODETEXT)) {
                        if(const wchar_t* t=(const wchar_t*)GlobalLock(g)) {
                            for(const wchar_t* p=t;*p && *p!=L'\\r' && *p!=L'\\n' && d->lNomeNuovaCartella.size()<48;++p)
                                if(*p>=32 && *p!=L'|' && *p!=L'/' && *p!=L'\\' && *p!=L':' && *p!=L'*' &&
                                   *p!=L'?' && *p!=L'"' && *p!=L'<' && *p!=L'>') d->lNomeNuovaCartella.push_back(*p);
                            GlobalUnlock(g);
                        }
                    }
                    CloseClipboard(); d->lDirty=true; return 0;
                }
                return 0;
            }
            if(w==27) {
                if(!d->query.empty()) {
                    d->query.clear(); d->filtra(); d->lModalita=0; d->lSelFile=-1;
                    d->lScroll=d->lScrollObj=0; d->lDirty=true;
                } else if(d->lModalita==1 || d->lModalita==2 || d->lModalita==3 || d->lModalita==5) {
                    d->lModalita=0; d->lNomeNuovoAttivo=false; d->lScrollEditor=0; d->lDirty=true;
                } else d->chiudiLauncher();
                return 0;
            }
            if(w==13) {
                if(d->lSelFile>=0) d->apriFile(d->lSelFile);
                else if(d->filtrate.empty() && !d->query.empty()) {
                    const int rows=d->lFileRighe();
                    d->apriFile(rows>0 && d->lFileTesto==d->query?0:rows);
                } else if(!d->filtrate.empty() && (d->lModalita==1 || d->lModalita==4))
                    d->avviaApp(d->lSel);
                return 0;
            }
            if(w==127 || (w==8 && (GetKeyState(VK_CONTROL)&0x8000))) {
                while(!d->query.empty() && d->query.back()==L' ') d->query.pop_back();
                while(!d->query.empty() && d->query.back()!=L' ') d->query.pop_back();
                d->filtra(); d->lModalita=d->query.empty()?0:4; d->lScroll=d->lScrollObj=0; return 0;
            }
            if(w==8) {
                if(!d->query.empty()) {
                    d->query.pop_back(); d->filtra(); d->lModalita=d->query.empty()?0:4;
                }
                d->lDirty=true; return 0;
            }
            if(w==22) {
                if(OpenClipboard(h)) {
                    if(HANDLE g=GetClipboardData(CF_UNICODETEXT)) {
                        if(const wchar_t* t=(const wchar_t*)GlobalLock(g)) {
                            for(const wchar_t* p=t;*p && *p!=L'\\r' && *p!=L'\\n' && d->query.size()<100;++p)
                                if(*p>=32) d->query+=*p;
                            GlobalUnlock(g);
                        }
                    }
                    CloseClipboard();
                    d->filtra(); d->lModalita=d->query.empty()?0:4; d->lScroll=d->lScrollObj=0;
                }
                return 0;
            }
            if(w>=32) {
                d->query+=(wchar_t)w; d->lModalita=4; d->filtra();
                d->lScroll=d->lScrollObj=0; d->lSelFile=-1; return 0;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if(d->lModalita==2 || ((d->lModalita==3 || d->lModalita==5) && d->lNomeNuovoAttivo)) return 0;
            // Frecce su e giu' fra griglia e file: dall'ultima riga di app (o senza app) la freccia giu' entra nei file;
            // nei file su e giu' scorrono le righe, fino a "Cerca in Esplora file"; su dalla prima torna alla griglia.
            // Gli altri tasti di spostamento riportano nella griglia
            {
                const int r = d->lFileRighe();
                if (r >= 0 && (w == VK_UP || w == VK_DOWN)) {
                    // (con righe di un testo vecchio si sceglie solo "Cerca in Esplora file": R20261119-195000-S2)
                    const bool validi = d->lFileTesto == d->query;
                    if (d->lSelFile >= 0) {
                        if (w == VK_DOWN) d->lSelFile = validi ? std::min(d->lSelFile + 1, r) : r;
                        else if (d->lSelFile > 0 && validi) d->lSelFile--;
                        else if (!d->filtrate.empty()) d->lSelFile = -1;
                        d->lDirty = true;
                        return 0;
                    }
                    if (w == VK_DOWN && (d->filtrate.empty() || d->lSel + (int)d->lColonne() >= (int)d->filtrate.size())) { d->lSelFile = validi ? 0 : r; d->lDirty = true; return 0; }
                }
                if (d->lSelFile >= 0 && (w == VK_LEFT || w == VK_RIGHT || w == VK_PRIOR || w == VK_NEXT || w == VK_HOME || w == VK_END)) {
                    d->lSelFile = -1;
                    if (d->filtrate.empty()) return 0;
                }
            }
            int s = d->lSel;
            // pagine: tante righe quante se ne vedono
            const int righePagina = std::max(1, (int)((d->lGrigliaBottom() - d->lGrigliaTop()) / d->lCellaH()));
            const int colonne = (int)d->lColonne();
            if (w == VK_LEFT) s -= d->lVistaLista ? 0 : 1;
            else if (w == VK_RIGHT) s += d->lVistaLista ? 0 : 1;
            else if (w == VK_UP) s -= colonne; else if (w == VK_DOWN) s += colonne;
            else if (w == VK_PRIOR) s -= colonne * righePagina; else if (w == VK_NEXT) s += colonne * righePagina;
            else if (w == VK_HOME) s = 0; else if (w == VK_END) s = (int)d->filtrate.size() - 1;
            else break;
            // senza ricerca la prima freccia mostra solo la scelta attuale; la scelta si vede come il passaggio del mouse
            if (d->query.empty() && d->lSopra != d->lSel) s = d->lSel;
            d->lSel = std::clamp(s, 0, std::max(0, (int)d->filtrate.size() - 1));
            if (d->query.empty() && !d->filtrate.empty()) d->lSopra = d->lSel;
            d->lSelTastiera = true; d->selezionaVisibile(); d->lDirty = true;
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) {
                // Appena aperto, il primo piano preso agganciandosi all'app di prima a volte torna a lei subito dopo
                // lo sgancio (come per la ricerca): non e' un clic altrove, si riprende una volta invece di chiudersi
                if (d->lAperto && !d->lChiusura && !d->lRipreso && adesso() - d->lT0 < 0.3 &&
                    d->primaDelLauncher && GetForegroundWindow() == d->primaDelLauncher) {
                    d->lRipreso = true;
                    d->scrivi("launcher: primo piano tornato all'app di prima, lo riprende");
                    PostMessageW(h, WM_APP + 3, 0, 0);
                    return 0;
                }
                // il fuoco va al dock perche' si e' cliccata l'icona App: la chiusura la fa quel clic (apriLauncher
                // fa da interruttore). Chiudendo qui, il clic subito dopo lo riaprirebbe
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && d->mouseSuIconaApp()) return 0;
                d->chiudiLauncher();
            }
            return 0;
        case WM_APP + 3: if (d->lAperto && !d->lChiusura) prendiFuoco(h); return 0;
        case WM_CLOSE: d->chiudiLauncher(); return 0;   // Alt+F4: come Esc (la finestra non si distrugge, si riusa)
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace dock
