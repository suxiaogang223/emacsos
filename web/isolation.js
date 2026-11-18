// GitHub Pages cannot set COOP/COEP headers. The adjacent service worker
// supplies them on navigation and reloads once before a machine can start.
window.coi = { coepCredentialless: () => false, coepDegrade: () => false };
