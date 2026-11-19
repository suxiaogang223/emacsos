// GitHub Pages cannot set COOP/COEP headers. The adjacent service worker
// supplies them on navigation and reloads once before a machine can start.
let isolationReloadPending = false;
window.coi = {
    coepCredentialless: () => false,
    coepDegrade: () => false,
    doReload: async () => {
        if (isolationReloadPending) return;
        isolationReloadPending = true;
        // updatefound can fire before activation. Reload only after the worker
        // can intercept this page's navigation and attach the isolation headers.
        await navigator.serviceWorker.ready;
        if (!navigator.serviceWorker.controller) {
            await new Promise(resolve => navigator.serviceWorker.addEventListener(
                'controllerchange', resolve, { once: true }));
        }
        window.location.reload();
    },
};
