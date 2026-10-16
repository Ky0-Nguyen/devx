// The same English/Vietnamese switch as the landing page, sharing its stored
// choice (localStorage "devx-lang") so the language follows the reader across
// the site. `?lang=en|vi` wins, for links that name a language.
(function () {
  var root = document.documentElement;
  function set(lang) {
    root.setAttribute('data-lang', lang);
    root.setAttribute('lang', lang);
    document.querySelectorAll('[data-set-lang]').forEach(function (b) {
      b.setAttribute('aria-pressed', String(b.getAttribute('data-set-lang') === lang));
    });
    var title = root.getAttribute('data-title-' + lang);
    if (title) document.title = title;
    try { localStorage.setItem('devx-lang', lang); } catch (e) {}
  }
  var asked = new URLSearchParams(location.search).get('lang');
  var saved = null;
  try { saved = localStorage.getItem('devx-lang'); } catch (e) {}
  var browser = (navigator.language || 'en').toLowerCase().indexOf('vi') === 0 ? 'vi' : 'en';
  set(asked === 'vi' || asked === 'en' ? asked : (saved === 'vi' || saved === 'en' ? saved : browser));
  document.querySelectorAll('[data-set-lang]').forEach(function (b) {
    b.addEventListener('click', function () { set(b.getAttribute('data-set-lang')); });
  });
})();
